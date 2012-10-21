/*
 * Tinyg_tc.c - TinyG temperature controller device
 * Part of TinyG project
 * Based on Kinen Motion Control System 
 *
 * Copyright (c) 2012 Alden S. Hart Jr.
 *
 * The Kinen Motion Control System is licensed under the OSHW 1.0 license
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY 
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>				// for memset
#include <avr/io.h>
#include <avr/interrupt.h>

#include "kinen_core.h"
#include "tinyg_tc.h"

// static functions 
static void _controller(void);
static uint8_t _idle_task(void);

// static data
static struct DeviceSingleton {
	uint8_t rtc_flag;			// true = the timer interrupt fired
	uint8_t rtc_100ms_count;	// 100ms down counter
	uint8_t rtc_1sec_count;		// 1 second down counter

} dev;

static uint8_t device_array[DEVICE_ADDRESS_MAX];


/****************************************************************************
 * main
 *
 *	Device and Kinen initialization
 *	Main loop handler
 */
int main(void)
{
	cli();						// initializations
	kinen_init();				// do this first
	device_init();				// handles all the device inits
	sei(); 						// enable interrupts

	device_unit_tests();		// uncomment __UNIT_TEST_DEVICE to enable unit tests

	while (true) {				// go to the controller loop and never return
		_controller();
	}
	return (false);				// never returns
}

/*
 * Dispatch loop
 *
 *	The dispatch loop is a set of pre-registered callbacks that (in effect) 
 *	provide rudimentry multi-threading. Functions are organized from highest
 *	priority to lowest priority. Each called function must return a status code
 *	(see kinen_core.h). If SC_EAGAIN (02) is returned the loop restarts at the
 *	start of the list. For any other status code exceution continues down the list
 */

#define	DISPATCH(func) if (func == SC_EAGAIN) return; 
static void _controller()
{
	DISPATCH(kinen_callback());	// intercept low-level communication events
	DISPATCH(rtc_callback());	// real-time clock handler
	DISPATCH(_idle_task());
}

static uint8_t _idle_task()
{
	return (SC_NOOP);
}

/**** Device Init ****
 */
void device_init(void)
{
	DDRB = PORTB_DIR;			// initialize all ports for proper IO
	DDRC = PORTC_DIR;
	DDRD = PORTD_DIR;

	rtc_init();
	pwm_init();
	led_on();					// put on the red light (Roxanne)
}


/**** PWM - Pulse Width Modulation Functions ****
 * pwm_init() 	  - initialize RTC timers and data
 * pwm_set_freq() - set PWM channel frequency
 * pwm_set_duty() - set PWM channel duty cycle 
 *
 *	Setting duty cycle to 0 disables the PWM channel with output low
 *	Setting duty cycle to 100 disables the PWM channel with output high
 *	Setting duty cycle between 0 and 100 enables PWM channel
 *
 *	The frequency must have been set previously
 */
void pwm_init(void)
{
	TCCR1A = 0;					// Initialize
	TCCR1A = 0;
/*
	while(true) {
		PINB = 0xFF;
		PINB = 0;
	}
*/
	// set comparator modes
//	TCCR1A |= 0b00000000;		// COM1A1, COM1A0	enable OCR1A only (reqs WGM mode 15)
	TCCR1A |= 0b11100000;		// COM1A1, COM1A0
	TCCR1A |= 0b00000000;		// COM1B1, COM1B0
	
	// set Waveform generation mode (WGM13 - WGM10)
	TCCR1A |= 0b00000011;		// WGM11, WGM 10	MODE 11
	TCCR1B |= 0b00010000;		// WGM13, WGM 12

//	TCCR1A |= 0b00000011;		// WGM11, WGM 10	MODE 15
//	TCCR1B |= 0b00011000;		// WGM13, WGM 12

	// set clock and prescaler 
	TCCR1B |= 0b00000011;		// CS12, CS11, CS10 - Fclk / 64

//    DDRB |= (1 << DDB1)|(1 << DDB2); // PB1 and PB2 is now an output

	// reset and configure timer 1, the Extruder Two PWM timer
	// Mode: Fast PWM with TOP=0xFF (8bit) (WGM3:0 = 0101), cycle freq= 976 Hz
	// Prescaler: 1/64 (250 KHz)
//	TCCR1A = 0b00000001;  
//	TCCR1B = 0b00001011;		// set to PWM mode

//	ICR1 = 0xFFF;				// set TOP to 16bit
	OCR1A = 0x400;				// set PWM for 25% duty cycle @ 16bit
	OCR1B = 0x020;				// set PWM for 75% duty cycle @ 16bit

//	TCCR1A |= (1 << COM1A1)|(1 << COM1B1); // set non-inverting mode

//	TCCR1A |= (1 << WGM11);
//	TCCR1B |= (1 << WGM12)|(1 << WGM13);	// set Fast PWM mode using ICR1 as TOP
//	TCCR1B |= (1 << CS10);		// START the timer with no prescaler

//	TCCR1A = (0x40 | 0x01);		// enable OCR1A only | fast PWM, 8bits (low bits)
//	TCCR1B = (0x08 | 0x04);		// fast PWM, 8bits (hi bits) | Clock / 256 prescaler
//	TCNT1 = (256 - RTC_10MS_COUNT);	// set timer for approx 10 ms overflow

	TIMSK1 = 0b00000110; 		// no interrupts needed	
}

ISR(TIMER1_COMPA_vect)
{
	PWM_PORT |= PWM_OUT0;
}

ISR(TIMER1_COMPB_vect)
{
	PWM_PORT &= ~PWM_OUT0;
}

uint8_t pwm_set_freq(uint8_t chan, double freq)
{
	
/*
	if (chan > PWMS) { return (TG_NO_SUCH_DEVICE);}
	if (freq > PWM_MAX_FREQ) { return (TG_INPUT_VALUE_TOO_SMALL);}
	if (freq < PWM_MIN_FREQ) { return (TG_INPUT_VALUE_TOO_LARGE);}

	// set the period and the prescaler
	double prescale = F_CPU/65536/freq;	// optimal non-integer prescaler value
	if (prescale <= 1) { 
		pwm[chan].timer->PER = F_CPU/freq;
		pwm[chan].timer->CTRLA = TC_CLKSEL_DIV1_gc;
	} else if (prescale <= 2) { 
		pwm[chan].timer->PER = F_CPU/2/freq;
		pwm[chan].timer->CTRLA = TC_CLKSEL_DIV2_gc;
	} else if (prescale <= 4) { 
		pwm[chan].timer->PER = F_CPU/4/freq;
		pwm[chan].timer->CTRLA = TC_CLKSEL_DIV4_gc;
	} else if (prescale <= 8) { 
		pwm[chan].timer->PER = F_CPU/8/freq;
		pwm[chan].timer->CTRLA = TC_CLKSEL_DIV8_gc;
	} else { 
		pwm[chan].timer->PER = F_CPU/64/freq;
		pwm[chan].timer->CTRLA = TC_CLKSEL_DIV64_gc;
	}
*/
	return (SC_OK);
}

uint8_t pwm_set_duty(uint8_t chan, double duty)
{
	if (duty < 0)   { return (SC_INPUT_VALUE_TOO_SMALL);}
	if (duty > 100) { return (SC_INPUT_VALUE_TOO_LARGE);}

//	pwm[chan].timer->CCB = (uint16_t)(pwm[chan].timer->PER - pwm[chan].timer->PER / (duty/100));
	return (SC_OK);
}

/**** RTC - Real Time Clock Functions ****
 * rtc_init() 	  - initialize RTC timers and data
 * ISR()		  - RTC interrupt routine 
 * rtc_callback() - run RTC from dispatch loop
 * rtc_10ms()	  - tasks that run every 10 ms
 * rtc_100ms()	  - tasks that run every 100 ms
 * rtc_1sec()	  - tasks that run every 100 ms
 */
void rtc_init(void)
{
	TCCR0A = 0x00;				// normal mode, no compare values
	TCCR0B = 0x05;				// normal mode, internal clock / 1024 ~= 7800 Hz
	TCNT0 = (256 - RTC_10MS_COUNT);	// set timer for approx 10 ms overflow
	TIMSK0 = (1<<TOIE0);		// enable overflow interrupts
	dev.rtc_100ms_count = 10;
	dev.rtc_1sec_count = 10;	
}

ISR(TIMER0_OVF_vect)
{
	TCNT0 = (256 - RTC_10MS_COUNT);	// reset timer for approx 10 ms overflow
	dev.rtc_flag = true;
}

uint8_t rtc_callback(void)
{
	if (dev.rtc_flag == false) { return (SC_NOOP);}
	dev.rtc_flag = false;

	rtc_10ms();

	if (--dev.rtc_100ms_count != 0) { return (SC_OK);}
	dev.rtc_100ms_count = 10;
	rtc_100ms();

	if (--dev.rtc_1sec_count != 0) { return (SC_OK);}
	dev.rtc_1sec_count = 10;
	rtc_1sec();

	return (SC_OK);
}

void rtc_10ms(void)
{
	led_toggle();
	return;
}

void rtc_100ms(void)
{
//	led_toggle();
	return;
}

void rtc_1sec(void)
{
//	led_toggle();
	return;
}

/**** LED Functions ****
 * led_on()
 * led_off()
 * led_toggle()
 */

void led_on(void) 
{
	LED_PORT &= ~(LED_PIN);
}

void led_off(void) 
{
	LED_PORT |= LED_PIN;
}

void led_toggle(void) 
{
	if (LED_PORT && LED_PIN) {
		led_on();
	} else {
		led_off();
	}
}

/****************************************************************************
 *
 * Kinen Callback functions - mandatory
 *
 *	These functions are called from Kinen drivers and must be implemented 
 *	for any Kinen device
 *
 *	device_reset() 		- reset device in response tro Kinen reset command
 *	device_read_byte() 	- read a byte from Kinen channel into device structs
 *	device_write_byte() - write a byte from device to Kinen channel
 */

void device_reset(void)
{
	return;
}

uint8_t device_read_byte(uint8_t addr, uint8_t *data)
{
	addr -= KINEN_COMMON_MAX;
	if (addr >= DEVICE_ADDRESS_MAX) return (SC_INVALID_ADDRESS);
	*data = device_array[addr];
	return (SC_OK);
}

uint8_t device_write_byte(uint8_t addr, uint8_t data)
{
	addr -= KINEN_COMMON_MAX;
	if (addr >= DEVICE_ADDRESS_MAX) return (SC_INVALID_ADDRESS);
	// There are no checks in here for read-only locations
	// Assumes all locations are writable.
	device_array[addr] = data;
	return (SC_OK);
}


//###########################################################################
//##### UNIT TESTS ##########################################################
//###########################################################################

#ifdef __UNIT_TEST_DEVICE

void device_unit_tests()
{

// success cases

// exception cases

}

#endif // __UNIT_TEST_DEVICE

