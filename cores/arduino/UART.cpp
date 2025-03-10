/*
  UART.cpp - Hardware serial library for Wiring
  Copyright (c) 2006 Nicholas Zambetti.  All right reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

  Created: 09.11.2017 07:29:09
  Author: M44307
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <util/atomic.h>
#include <avr/io.h>
#include "Arduino.h"

#include "UART.h"
#include "UART_private.h"

// this next line disables the entire UART.cpp,
// this is so I can support Attiny series and any other chip without a uart
#if defined(HAVE_HWSERIAL0) || defined(HAVE_HWSERIAL1) || defined(HAVE_HWSERIAL2) || defined(HAVE_HWSERIAL3)

// SerialEvent functions are weak, so when the user doesn't define them,
// the linker just sets their address to 0 (which is checked below).
// The Serialx_available is just a wrapper around Serialx.available(),
// but we can refer to it weakly so we don't pull in the entire
// UART instance if the user doesn't also refer to it.
#if defined(HAVE_HWSERIAL0)
void serialEvent() __attribute__((weak));
bool Serial0_available() __attribute__((weak));
#endif

#if defined(HAVE_HWSERIAL1)
void serialEvent1() __attribute__((weak));
bool Serial1_available() __attribute__((weak));
#endif

#if defined(HAVE_HWSERIAL2)
void serialEvent2() __attribute__((weak));
bool Serial2_available() __attribute__((weak));
#endif

#if defined(HAVE_HWSERIAL3)
void serialEvent3() __attribute__((weak));
bool Serial3_available() __attribute__((weak));
#endif

void serialEventRun(void)
{
#if defined(HAVE_HWSERIAL0)
    if (Serial0_available && serialEvent && Serial0_available()) serialEvent();
#endif
#if defined(HAVE_HWSERIAL1)
    if (Serial1_available && serialEvent1 && Serial1_available()) serialEvent1();
#endif
#if defined(HAVE_HWSERIAL2)
    if (Serial2_available && serialEvent2 && Serial2_available()) serialEvent2();
#endif
#if defined(HAVE_HWSERIAL3)
    if (Serial3_available && serialEvent3 && Serial3_available()) serialEvent3();
#endif
}

// macro to guard critical sections when needed for large RX and TX buffer sizes
#if (SERIAL_RX_BUFFER_SIZE > 256)
#define RX_BUFFER_ATOMIC ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
#else
#define RX_BUFFER_ATOMIC
#endif
#if (SERIAL_TX_BUFFER_SIZE > 256)
#define TX_BUFFER_ATOMIC ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
#else
#define TX_BUFFER_ATOMIC
#endif

// Actual interrupt handlers //////////////////////////////////////////////////////////////

void UartClass::_tx_data_empty_irq(void)
{
    // There must be more data in the output
    // buffer. Send the next byte
    unsigned char c = _tx_buffer[_tx_buffer_tail];
    _tx_buffer_tail = (_tx_buffer_tail + 1) % SERIAL_TX_BUFFER_SIZE;

    (*_hwserial_module).TXDATAL = c;

    // clear the TXCIF flag -- "can be cleared by writing a one to its bit
    // location". This makes sure flush() won't return until the bytes
    // actually got written
    (*_hwserial_module).STATUS = USART_TXCIF_bm;

    if (_tx_buffer_head == _tx_buffer_tail) {
        // Buffer empty, so disable "data register empty" interrupt
        (*_hwserial_module).CTRLA &= (~USART_DREIE_bm);
    }
}

// To invoke data empty "interrupt" via a call, use this method
void UartClass::_poll_tx_data_empty(void) 
{
    // Note: Testing the SREG I-bit here would only check if interrupts are disabled
    // globally, and would not establish if this call was via an interrupt of some 
    // description. It is thus better to turn off interrupts globally (using an
    // ATOMIC BLOCK) and always poll the DRE bits

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        // Call the handler only if data register is empty and we know the buffer is non-empty
        // by checking the status of the corresponding interrupt enable. 
        // Note that the re-check of DREIE within the zone is required although it may have
        // been checked earlier.
        if (((*_hwserial_module).CTRLA & USART_DREIE_bm) && ((*_hwserial_module).STATUS & USART_DREIF_bm)) {
            _tx_data_empty_irq();
        }
    }
}

// Public Methods //////////////////////////////////////////////////////////////

void UartClass::begin(unsigned long baud, uint16_t config)
{
    // Make sure no transmissions are ongoing and USART is disabled in case begin() is called by accident
    // without first calling end()
    if(_written) {
        this->end();
        _written = false;
    }

    int32_t baud_setting = (((8 * F_CPU) / baud) + 1) / 2;
    int8_t sigrow_val = SIGROW.OSC16ERR5V;
    baud_setting += (baud_setting * sigrow_val) / 1024;

    //Make sure global interrupts are disabled during initialization
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {

        // Setup port mux
        PORTMUX.USARTROUTEA |= _uart_mux;

        //Set up the rx pin before we enable the receiver
        pinMode(_hwserial_rx_pin, INPUT_PULLUP);
        digitalWrite(_hwserial_tx_pin, HIGH);

        // Disable CLK2X
        (*_hwserial_module).CTRLB &= (~USART_RXMODE_CLK2X_gc);
        (*_hwserial_module).CTRLB |= USART_RXMODE_NORMAL_gc;

        // assign the baud_setting, a.k.a. BAUD (USART Baud Rate Register)
        (*_hwserial_module).BAUD = (int16_t) baud_setting;

        // Set USART mode of operation
        (*_hwserial_module).CTRLC = config;

        // Enable transmitter and receiver
        (*_hwserial_module).CTRLB |= (USART_RXEN_bm | USART_TXEN_bm);

        (*_hwserial_module).CTRLA |= USART_RXCIE_bm;

        //Enable the tx pin after we enable transmitter
        pinMode(_hwserial_tx_pin, OUTPUT);
    }
}

void UartClass::end()
{
    // wait for transmission of outgoing data
    flush();

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        // Disable receiver and transmitter as well as the RX complete and
        // data register empty interrupts.
        (*_hwserial_module).CTRLB &= ~(USART_RXEN_bm | USART_TXEN_bm);
        (*_hwserial_module).CTRLA &= ~(USART_RXCIE_bm | USART_DREIE_bm);
        // clear any received data not read yet
        _rx_buffer_head = _rx_buffer_tail;

        _written = false;
    }
    // Note: Does not change output pins
}

int UartClass::available(void)
{
    int n;
    RX_BUFFER_ATOMIC {
        n = ((unsigned int)(SERIAL_RX_BUFFER_SIZE + _rx_buffer_head - _rx_buffer_tail)) % SERIAL_RX_BUFFER_SIZE;
    }
    return n;
}

int UartClass::peek(void)
{
    int c;
    RX_BUFFER_ATOMIC {
        if (_rx_buffer_head == _rx_buffer_tail) {
            c = -1;
        } else {
            c = _rx_buffer[_rx_buffer_tail];
        }
    }
    return c;
}

int UartClass::read(void)
{
    int c;
    RX_BUFFER_ATOMIC {
        // if the head isn't ahead of the tail, we don't have any characters
        if (_rx_buffer_head == _rx_buffer_tail) {
            c = -1;
        } else {
            c = _rx_buffer[_rx_buffer_tail];
            _rx_buffer_tail = (rx_buffer_index_t)(_rx_buffer_tail + 1) % SERIAL_RX_BUFFER_SIZE;
        }
    }
    return c;
}

int UartClass::availableForWrite(void)
{
    tx_buffer_index_t head;
    tx_buffer_index_t tail;

    TX_BUFFER_ATOMIC {
        head = _tx_buffer_head;
        tail = _tx_buffer_tail;
    }
    if (head >= tail) return SERIAL_TX_BUFFER_SIZE - 1 - head + tail;
    return tail - head - 1;
}

void UartClass::flush()
{
    // If we have never written a byte, no need to flush. This special
    // case is needed since there is no way to force the TXCIF (transmit
    // complete) bit to 1 during initialization
    if (!_written) {
        return;
    }

    // Spin until the data-register-empty-interrupt is disabled and TX complete interrupt flag is raised
    while ( ((*_hwserial_module).CTRLA & USART_DREIE_bm) || (!((*_hwserial_module).STATUS & USART_TXCIF_bm)) ) {

        // If interrupts are globally disabled or the and DR empty interrupt is disabled,
        // poll the "data register empty" interrupt flag to prevent deadlock
        _poll_tx_data_empty();
    }
    // If we get here, nothing is queued anymore (DREIE is disabled) and
    // the hardware finished transmission (TXCIF is set).
}

size_t UartClass::write(uint8_t c)
{
    for (;;) {
        // If the buffer and the data register is empty, just write the byte
        // to the data register and be done. This shortcut helps
        // significantly improve the effective data rate at high (>
        // 500kbit/s) bit rates, where interrupt overhead becomes a slowdown.
        // Note also that USART_DREIE_bm always will be clear if the buffer is
        // empty.
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
            if ( !((*_hwserial_module).CTRLA & USART_DREIE_bm) && ((*_hwserial_module).STATUS & USART_DREIF_bm) ) {
                (*_hwserial_module).TXDATAL = c;
                (*_hwserial_module).STATUS = USART_TXCIF_bm;
                _written = true;
                return 1;
            }

            // ...if we want to reduce the length of the critical zone, we could interrupt it here...

            tx_buffer_index_t nexthead = (_tx_buffer_head + 1) % SERIAL_TX_BUFFER_SIZE;
            if (nexthead != _tx_buffer_tail) {
                _tx_buffer[_tx_buffer_head] = c;
                _tx_buffer_head = nexthead;
                // Enable data "register empty interrupt" if it was not already
                // (not atomic)
                (*_hwserial_module).CTRLA |= USART_DREIE_bm;
                return 1;
            }
        }

        // The output buffer is full, so there's nothing else to do than to spin
        // here waiting for some room in the buffer to become available
        // Note that USART_DREIE_bm is assumed to be set at this time
        _poll_tx_data_empty();
    }
}

#endif // whole file
