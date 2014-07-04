/*
 * OrmerodLedSensor.cpp
 *
 * Created: 16/02/2014 21:15:50
 *  Author: David
 */ 

#include <ecv.h>

#ifdef __ECV__
#define __attribute__(_x)
#define __volatile__
#define __DOXYGEN__				// this avoids getting the wrong definitions for uint32_t etc.
#endif

#ifdef __ECV__
#pragma ECV noverifyincludefiles
#endif

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/eeprom.h>
#include <avr/wdt.h>

#ifdef __ECV__
#pragma ECV verifyincludefiles
#endif

#define ISR_DEBUG	(0)		// set nonzero to use PA4 as debug output pin

#define BITVAL(_x) (1u << (_x))

// Pin assignments:
// PA0/ADC0		digital input from Duet
// PA1/ADC1		analog input from phototransistor
// PA2/ADC2		output to Duet via 13K resistor
// PA3/ADC3		analog input from thermistor
// PA4/SCLK		can be tied to ground to indicate that a 4k7 thermistor series resistor is used instead of 1k. Also SCLK when programming.
// PA5/OC1B		digital output to control fan, active high. Also MISO when programming.
// PA6/OC1A		near LED drive, active low in prototype, will be active high in release. Also MOSI when programming.
// PA7/OC0B		output to Duet via 10K resistor

// PB0			far LED drive, active high
// PB1			far LED drive, active high (paralleled with PB0)
// PB2/OC0A		unused
// PB3/RESET	not available, used for programming

__fuse_t __fuse __attribute__((section (".fuse"))) = {0xE2, 0xDF, 0xFF};

const unsigned int PortADuetInputBit = 0;
const unsigned int AdcPhototransistorChan = 1;
const unsigned int PortADuet13KOutputBit = 2;
const unsigned int AdcThermistorChan = 3;
const unsigned int PortASeriesResistorSenseBit = 4;
const unsigned int PortAFanControlBit = 5;
const unsigned int PortANearLedBit = 6;
const unsigned int PortADuet10KOutputBit = 7;

const uint8_t PortAUnusedBitMask = 0;

const uint8_t PortBFarLedMask = BITVAL(0) | BITVAL(1);
#if ISR_DEBUG
const unsigned int PortBDebugPin = 2;
const uint8_t PortBUnusedBitMask = 0;
#else
const uint8_t PortBUnusedBitMask = BITVAL(2);
#endif

// Approximate MPU frequency (8MHz internal oscillator)
const uint32_t F_CPU = 8000000uL;

// IR parameters. These also allow us to receive a signal through the command input.
const uint16_t interruptFreq = 8000;						// interrupt frequency. We run the IR sensor at one quarter of this, i.e. 2kHz
															// highest usable value is about 9.25kHz because the ADC needs 13.5 clock cycles per conversion.
const uint16_t divisorIR = (uint16_t)(F_CPU/interruptFreq);
const uint16_t baseTopIR = divisorIR - 1;
const uint16_t cyclesAveragedIR = 8;						// must be a power of 2, max 32 (unless we make onSumIR and offSumIR uint32_t)

const uint16_t farThreshold = 10 * cyclesAveragedIR;		// minimum far reading for us to think the sensor is working properly
const uint16_t simpleNearThreshold = 30 * cyclesAveragedIR;	// minimum reading to set output high in simple mode
const uint16_t saturatedThreshold = 870 * cyclesAveragedIR;	// minimum reading for which we consider the sensor saturated

// IR variables
typedef uint8_t invariant(value < cyclesAveragedIR) irIndex_t;
	
volatile uint16_t nearSumIR, farSumIR, offSumIR;
uint16_t nearLedReadings[cyclesAveragedIR], farLedReadings[cyclesAveragedIR], offReadings[cyclesAveragedIR];
irIndex_t nearLedIndex, farLedIndex, offIndex;

ghost(
	bool irInvariant()
		returns(   (forall r in nearLedReadings :- r <= 1023)
				&& (forall r in farLedReadings :- r <= 1023)
				&& (forall r in offReadings :- r <= 1023)
				&& nearSumIR == + over nearLedReadings
				&& farSumIR == + over farLedReadings
				&& offSumIR == + over offReadings
			   );
)
			   
// Fan parameters
const uint16_t fanSampleFreq = 16;
const uint16_t fanSampleIntervalTicks = interruptFreq/fanSampleFreq;
const uint16_t fanSamplesAveraged = 16;

// Fan thresholds

// Using a 1K series resistor, we use the ADC in differential mode with a gain of 1 and full scale resolution of 512, but we reverse the inputs so we effectively get 2x extra gain.
const uint16_t thermistorConnectedThreshold1K = 30;	// we consider the hot end thermistor to be disconnected if we get this reading less than the reference, or a higher reading
const uint16_t thermistorOffThreshold1K = 340;			// we turn the fan off if we get this reading less than the reference (about 38C), or a higher reading
const uint16_t thermistorOnThreshold1K = 400;			// we turn the fan on if we get this reading less than the reference (about 42C), or lower reading

// Using a 4K7 series resistor, we use the ADC in single-ended mode with a gain of 1.
const uint16_t thermistorConnectedThreshold4K7 = 7;		// we consider the hot end thermistor to be disconnected if we get this reading less than the reference, or a higher reading
const uint16_t thermistorOffThreshold4K7 = 78;			// we turn the fan off if we get this reading less than the reference (about 38C), or a higher reading
const uint16_t thermistorOnThreshold4K7 = 92;			// we turn the fan on if we get this reading less than the reference (about 42C), or lower reading

const uint16_t fanOnSeconds = 2;						// once the fan is on, we keep it on for at least this time

// Fan variables
typedef uint8_t invariant(value < fanSamplesAveraged) fanIndex_t;

uint16_t fanReadings[fanSamplesAveraged], fanOffsets[fanSamplesAveraged];
volatile uint16_t fanReadingSum, fanOffsetSum;
uint16_t thermistorConnectedThreshold, thermistorOnThreshold, thermistorOffThreshold;
fanIndex_t fanIndex;
bool fan1Kmode;
uint16_t lastFanSampleTicks;
uint8_t fanChangeCount;

ghost(
	bool fanInvariant()
		returns(   (forall r in fanReadings :- r <= 1023)
				&& (forall r in fanOffsets :- r <= 1023)
				&& fanReadingSum == + over fanReadings
				&& fanOffsetSum == + over fanOffsets
			   );
)

// General variables
volatile uint16_t tickCounter;						// counts system ticks, lower 2 bits also used for ADC/LED state
bool running;


// ISR for the timer 0 compare match A interrupt
// This works on a cycle of 8 readings as follows:
// leds off, far led on, near led on, leds off, far led on, near led on, discarded fan reading, fan reading
// The reason for this is that when we switch the ADC into differential mode to get reliable readings with a 1K series resistor,
// the ADC subsystem needs extra time to settle down.
ISR(TIM1_COMPB_vect)
writes(nearLedReadings; nearLedIndex; farLedReadings; farLedIndex; offReadings; offIndex; fanReadings; fanOffsets; fanIndex; volatile)
pre(irInvariant())
pre(fanInvariant())
post(irInvariant())
post(fanInvariant())
{
#if ISR_DEBUG
	PORTB |= BITVAL(PortBDebugPin);					// set debug pin high
#endif

	uint16_t adcVal = ADC & 1023u;					// get the ADC reading from the previous conversion
	uint8_t locTickCounter = (uint8_t)tickCounter;
	while (TCNT1 < 3 * 64) {}						// delay a little until the ADC s/h has taken effect. 3 ADC clocks should be enough.
	switch(locTickCounter & 0x0fu)
	{
		case 0:										
			// LEDs are off, we just did a fan reading, we are doing an off reading now and a far reading next
			if (running)
			{
				if (fan1Kmode)
				{
					adcVal ^= 0x0200u;				// convert signed reading to unsigned biased by 512
					if ((locTickCounter & 0x10u) == 0)
					{
						fanReadingSum = fanReadingSum - fanReadings[fanIndex] + adcVal;
						fanReadings[fanIndex] = adcVal;
						fanIndex = (fanIndex + 1) & (fanSamplesAveraged - 1);
					}
					else
					{
						fanOffsetSum = fanOffsetSum - fanOffsets[fanIndex] + adcVal;
						fanOffsets[fanIndex] = adcVal;
					}
				}
				else
				{
					fanReadingSum = fanReadingSum - fanReadings[fanIndex] + adcVal;
					fanReadings[fanIndex] = adcVal;
					fanIndex = (fanIndex + 1) & (fanSamplesAveraged - 1);
				}
			}
			PORTB |= PortBFarLedMask;				// turn far LED on
			break;
			
		case 3:
		case 6:
		case 9:
			// LEDs are off, we just did a near reading, we are doing an off reading now and a far reading next
			if (running)
			{
				nearSumIR = nearSumIR - nearLedReadings[nearLedIndex] + adcVal;
				nearLedReadings[nearLedIndex] = adcVal;
				nearLedIndex = (nearLedIndex + 1) & (cyclesAveragedIR - 1);
			}
			PORTB |= PortBFarLedMask;				// turn far LED on
			break;

		case 1:
		case 4:
		case 7:
		case 10:
			// Far LED is on, we just did an off reading, we are doing a far reading now and a near reading next
			if (running)
			{
				offSumIR = offSumIR - offReadings[offIndex] + adcVal;
				offReadings[offIndex] = adcVal;
				offIndex = (offIndex + 1) & (cyclesAveragedIR - 1);
			}
			PORTB &= ~PortBFarLedMask;				// turn far LED on
			PORTA |= BITVAL(PortANearLedBit);		// turn near LED on
			break;
		
		case 2:
		case 5:
		case 8:
			// Near LED is on, we just did a far reading, we are doing a near reading now and an off reading next			
			if (running)
			{
				farSumIR = farSumIR - farLedReadings[farLedIndex] + adcVal;
				farLedReadings[farLedIndex] = adcVal;
				farLedIndex = (farLedIndex + 1) & (cyclesAveragedIR - 1);
			}
			PORTA &= ~BITVAL(PortANearLedBit);		// turn near LED off
			break;
					
		case 11:
			// Near LED is on, we just did a far reading, we are doing a near reading now and a fan reading next
			if (running)
			{
				farSumIR = farSumIR - farLedReadings[farLedIndex] + adcVal;
				farLedReadings[farLedIndex] = adcVal;
				farLedIndex = (farLedIndex + 1) & (cyclesAveragedIR - 1);
			}
			PORTA &= ~BITVAL(PortANearLedBit);		// turn near LED off
			if (fan1Kmode)
			{
				ADMUX = ((locTickCounter & 0x10u) != 0)
						? 0b00010011		// +ve input = ADC3, -ve input = ADC4, gain x20
						: 0b00110011;		// +ve input = ADC4, -ve input = ADC3, gain x20
			}
			else
			{
				ADMUX = BITVAL(MUX1) | BITVAL(MUX0);	// select fan as a single-ended input
			}						
			break;
		
		case 12:
			// LEDs are off, we just did a near reading, we are doing a fan reading which we will discard, we will do another fan reading next
			if (running)
			{
				nearSumIR = nearSumIR - nearLedReadings[nearLedIndex] + adcVal;
				nearLedReadings[nearLedIndex] = adcVal;
				nearLedIndex = (nearLedIndex + 1) & (cyclesAveragedIR - 1);
			}
			break;
			
		case 13:
		case 14:
			// LEDs are off and we are doing dummy fan readings
			break;

		case 15:
			// LEDs are off, we just did a dummy fan reading, we are doing another fan reading now and an off reading next
			ADMUX = BITVAL(MUX0);					// select input 1 = phototransistor
			break;
	}
	
	++tickCounter;

#if ISR_DEBUG
	PORTB &= (uint8_t)(~BITVAL(PortBDebugPin) & 0xFFu);			// set debug pin high
#endif
}

// Delay for a little while
// Each iteration of the loop takes 4 clocks plus one clock per NOP instruction in the body.
// The additional overhead for the function, including calling it, is 12 clocks.
// Therefore, with F_CPU = 8MHz and using 4 NOP instructions, it delays n + 1.5 microseconds.
void shortDelay(uint8_t n)
{
	for (uint8_t i = 0; i < n; ++i)
	keep(i <= n)
	decrease(n - i)
	{
#ifndef __ECV__			// eCv doesn't understand asm
		asm volatile ("nop");
		asm volatile ("nop");
		asm volatile ("nop");
		asm volatile ("nop");
#endif
	}
}

// Check the fan
void checkFan()	
writes(fanChangeCount; volatile)
{
	if ((PORTA & BITVAL(PortAFanControlBit)) != 0)
	{
		// Fan is on. Turn it off if thermistor is connected and temp <= off-threshold
		if (fanReadingSum < fanOffsetSum)
		{
			uint16_t fanDiff = fanOffsetSum - fanReadingSum;
			if (fanDiff >= thermistorConnectedThreshold && fanDiff <= thermistorOffThreshold)
			{
				if (fanChangeCount == 0)
				{
					PORTA &= (uint8_t)~BITVAL(PortAFanControlBit);	// turn fan off						
				}
				else
				{
					--fanChangeCount;
				}
			}
		}			
	}
	else
	{
		uint16_t fanDiff;
		// Fan is off. Turn it on if thermistor is disconnected or temp >= on-threshold
		if (fanReadingSum >= fanOffsetSum
			|| (fanDiff = fanOffsetSum - fanReadingSum) < thermistorConnectedThreshold
			|| fanDiff >= thermistorOnThreshold)
		{
			PORTA |= BITVAL(PortAFanControlBit);			// turn fan on
			fanChangeCount = (fanOnSeconds * fanSampleFreq) - 1;
		}		
	}
	
#ifndef __ECV__
	wdt_reset();											// kick the watchdog
#endif
}

// Give a G31 reading of about 0
inline void SetOutputOff()
writes(volatile)
{
	// We do this is 2 operations, each of which is atomic, so that we don't mess up what the ISR is doing with the LEDs.
	PORTA &= ~BITVAL(PortADuet10KOutputBit);
	PORTA &= ~BITVAL(PortADuet13KOutputBit);
}

// Give a G31 reading of about 445 indicating we are approaching the trigger point
inline void SetOutputApproaching()
writes(volatile)
{
	// We do this is 2 operations, each of which is atomic, so that we don't mess up what the ISR is doing with the LEDs.
	PORTA &= ~BITVAL(PortADuet10KOutputBit);
	PORTA |= BITVAL(PortADuet13KOutputBit);
}	

// Give a G31 reading of about 578 indicating we are at/past the trigger point
inline void SetOutputOn()
writes(volatile)
{
	// We do this is 2 operations, each of which is atomic, so that we don't mess up what the ISR is doing with the LEDs.
	PORTA &= ~BITVAL(PortADuet13KOutputBit);
	PORTA |= BITVAL(PortADuet10KOutputBit);
}

// Give a G31 reading of about 1023 indicating that the sensor is saturating
inline void SetOutputSaturated()
writes(volatile)
{
	// We do this is 2 operations, each of which is atomic, so that we don't mess up what the ISR is doing with the LEDs.
	PORTA |= BITVAL(PortADuet10KOutputBit);
	PORTA |= BITVAL(PortADuet13KOutputBit);
}

// Run the IR sensor and the fan
void runIRsensorAndFan()
writes(running; nearLedReadings; farLedReadings; offReadings; nearLedIndex; farLedIndex; offIndex; volatile)
writes(fanChangeCount; lastFanSampleTicks)
pre(fanInvariant())
{
	running = false;

	for (uint8_t i = 0; i < cyclesAveragedIR; ++i)
	keep(i <= cyclesAveragedIR)
	keep(forall j in 0..(i-1) :- nearLedReadings[j] == 0 && farLedReadings[j] == 0 && offReadings[j] == 0)
	decrease(cyclesAveragedIR - i)
	{
		nearLedReadings[i] = farLedReadings[i] = offReadings[i] = 0;		
	}
	nearLedIndex = farLedIndex = offIndex = 0;
	nearSumIR = farSumIR = offSumIR = 0;
	assert(irInvariant());

	cli();
	// Set up timer 1 in mode 12
	TCCR1A = 0;												// no direct outputs
	TCCR1B = BITVAL(WGM13) | BITVAL(WGM12);					// set the mode, clock stopped for now
	TCCR1C = 0;
	TCNT1 = 0;
	ICR1 = baseTopIR;
	OCR1B = 0;
	TIFR1 = BITVAL(OCF1B);									// clear any pending interrupt
	TIMSK1 = BITVAL(OCIE1B);								// enable the timer 0 compare match B interrupt
	TCCR1B |= BITVAL(CS10);									// start the clock
	
	ADMUX = BITVAL(MUX0);									// select input 1, single-ended mode
	ADCSRA = BITVAL(ADEN) | BITVAL(ADATE) | BITVAL(ADPS2) | BITVAL(ADPS1);	// enable ADC, auto trigger enable, prescaler = 64 (ADC clock ~= 125kHz)
	ADCSRB = BITVAL(ADTS2) | BITVAL(ADTS0) | BITVAL(BIN);	// start conversion on timer 1 compare match B, bipolar input mode when using differential inputs
	tickCounter = 0;
	sei();
	
	while (tickCounter < 4) {}								// ignore the readings from the first few interrupts after changing mode
	running = true;											// tell interrupt handler to collect readings
	lastFanSampleTicks = 0;

	for (;;)
	keep(irInvariant())
	keep(fanInvariant())
	{
		cli();
		uint16_t locNearSum = nearSumIR;
		uint16_t locFarSum = farSumIR;
		uint16_t locOffSum = offSumIR;
		sei();
			
		if (locNearSum >= saturatedThreshold || locFarSum >= saturatedThreshold)
		{
			SetOutputSaturated();							// sensor is saturating, so set the output full on to indicate this
		}
		else
		{
			locNearSum = (locNearSum > locOffSum) ? locNearSum - locOffSum : 0;
			locFarSum = (locFarSum > locOffSum) ? locFarSum - locOffSum : 0;
			
			if ((PINA & BITVAL(PortADuetInputBit)) == 0)
			{
				// Backup mode (simple modulated IR sensor mode), suitable for x-endstop detection.
				// We use only the near reading, because the far one can be high at too long a range.
				if (locNearSum >= simpleNearThreshold)
				{
					SetOutputOn();
				}
				else
				{
					// Don't give an 'approaching' reading when using the simple sensor, to help confirm which sensor we are using
					SetOutputOff();
				}
			}
			else
			{
				// Differential modulated IR sensor mode								
				if (locNearSum > locFarSum && locFarSum >= farThreshold)
				{
					SetOutputOn();
				}
				else if (locFarSum >= farThreshold && locNearSum * 6UL >= locFarSum * 5UL)
				{
					SetOutputApproaching();
				}
				else
				{
					SetOutputOff();
				}
			}			
		}
		
		// Check whether we need to poll the fan
		cli();
		uint16_t locTickCounter = tickCounter;
		sei();
		if (locTickCounter - lastFanSampleTicks >= fanSampleIntervalTicks)
		{
			checkFan();
			lastFanSampleTicks += fanSampleIntervalTicks;
		}
	}
}


// Main program
int main(void)
writes(volatile)
writes(running)
writes(nearLedReadings; farLedReadings; offReadings; nearLedIndex; farLedIndex; offIndex)	// IR variables
writes(fanReadings; fanOffsets; fanIndex; fanChangeCount; lastFanSampleTicks)				// fan variables
writes(fan1Kmode; thermistorConnectedThreshold; thermistorOffThreshold; thermistorOnThreshold)
{
	cli();
	DIDR0 = BITVAL(AdcPhototransistorChan) | BITVAL(AdcThermistorChan);
															// disable digital input buffers on ADC pins
	// Set ports and pullup resistors
	PORTA = BITVAL(PortADuetInputBit) | BITVAL(PortASeriesResistorSenseBit) | PortAUnusedBitMask;
															// enable pullup on Duet input, series resistor sense input, and unused I/O pins
	PORTB = PortBUnusedBitMask;								// enable pullup on unused I/O pins
	
	// Enable outputs
	DDRA = BITVAL(PortAFanControlBit) | BITVAL(PortANearLedBit) | BITVAL(PortADuet10KOutputBit) | BITVAL(PortADuet13KOutputBit);
#if ISR_DEBUG
	DDRB = PortBFarLedMask | BITVAL(PortBDebugPin);			// enable LED and debug outputs on port B
#else
	DDRB = PortBFarLedMask;									// enable LED outputs on port B
#endif
	
	// Wait 10ms to ensure that the power has stabilized before we read the series resistor sense pin
	for (uint8_t i = 0; i < 40; ++i)
	{
		shortDelay(255);
	}
	fan1Kmode = ((PINA & BITVAL(PortASeriesResistorSenseBit)) != 0);
	
	// Initialize the fan so that it won't come on at power up
	uint16_t readingInit, offsetInit;
	if (fan1Kmode)
	{
		// When reading the thermistor, we run ADC in differential bipolar mode, gain = 20 (effective gain is 10 because full-scale is 512 not 1024)
		thermistorConnectedThreshold = thermistorConnectedThreshold1K * fanSamplesAveraged;
		thermistorOffThreshold = thermistorOffThreshold1K * fanSamplesAveraged;
		thermistorOnThreshold = thermistorOnThreshold1K * fanSamplesAveraged;
		offsetInit = 512;
		readingInit = offsetInit - thermistorConnectedThreshold1K;
	}
	else
	{
		// When reading the thermistor, we run ADC in single-ended mode, gain = 1
		thermistorConnectedThreshold = thermistorConnectedThreshold4K7 * fanSamplesAveraged;
		thermistorOffThreshold = thermistorOffThreshold4K7 * fanSamplesAveraged;
		thermistorOnThreshold = thermistorOnThreshold4K7 * fanSamplesAveraged;
		offsetInit = 1023;
		readingInit = offsetInit - thermistorConnectedThreshold4K7;
	}

	fanReadingSum = 0;
	fanOffsetSum = 0;

	for (uint8_t i = 0; i < fanSamplesAveraged; ++i)
	keep(i <= fanSamplesAveraged)
	keep(forall j in 0..(i - 1) :- fanReadings[j] == readingInit)
	keep(forall j in 0..(i - 1) :- fanOffsets[j] == offsetInit)
	keep(fanReadingSum == + over fanReadings.take(i))
	keep(fanOffsetSum == + over fanReadings.take(i))
	decrease(fanSamplesAveraged - i)
	{
		fanReadings[i] = readingInit;
		fanReadingSum += readingInit;		
		fanOffsets[i] = offsetInit;
		fanOffsetSum += offsetInit;
	}
	fanIndex = 0;
	assert(fanInvariant());
	
	sei();

#ifndef __ECV__												// eCv++ doesn't understand gcc assembler syntax
	wdt_enable(WDTO_500MS);									// enable the watchdog (we kick it when checking the fan)	
#endif
		
	runIRsensorAndFan();									// doesn't return
	return 0;												// to keep gcc happy
}