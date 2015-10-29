/**
 * RS485 protocol library.
 * Adam Baron - vysocan 2011.  
 * 
 * For Atmega168, 328
 * 
 * 0.90 Beta  
 *      Tested on two breadboards, sending read data or commands looks fine. 
 *      Sending junk random data looks fine. One of many is picked up as general
 *      command. I guess when the random 2 bytes matches random xor. No random data is
 *      pushed through 2 byte crc.
 * 0.91 01.03.2014 vysocan
 *      Modified for NILRTOS.
 *      Replaced custom CRC by util/crc16.h
 *      atmega1284P, second UART
 *      Tested on final hardware, fixed few small bug, working fine
 *  
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * 
 * Packet structure: 
 * -------------~~~~~~~~~~~~~~~~~~~~~~
 * - A - P - X -  D0 - D63 ~ S0 ~ S0 ~
 * -------------~~~~~~~~~~~~~~~~~~~~~~
 * 
 * - Required fields
 * ~ Data and signature fields
 * A Address - 4 bits form and 4 bits to address.
 *   0  is master, but not really as any node can drive communication
 *   15 is broadcast to all
 *   14 addresses left for other devices
 * 
 * P Packet definition - 2 bits type and 6 bits length.
 *   Flag bit configuration:
 *   FLAG_ACK 3
 *   FLAG_NAK 2
 *   FLAG_CMD 1
 *   FLAG_DTA 0
 *    
 *   Types: CMD - Command - user defined general commands value 0 - 63
 *          DTA - Data - user data length D0 - D63 and CRC S0, S1
 *          ACK, NAK - flags for data, using signature of data packet
 *          
 * X XOR of A and P, for basic transfer safety.
 * 
 * D0 - D63 user data.     
 *                      
 * S0 ~ S0  CRC for user data or signature for ACK, NAK.  
 * 
 *
 * Application notes:
 * Baud rate should be bigger then 9600bps for a 16 MHz crystal, due
 * to stochastic wait for empty line to work properly.
 *       
 */
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/crc16.h>
#include <stdlib.h>


#include <NilSerial.h>
#define Serial NilSerial

#include "NilRS485.h"

// Declare and initialize the semaphore.
static SEMAPHORE_DECL(RS485NewMsg, 0);

// macros
#define output_low(port,pin) port &= ~(1<<pin)
#define output_high(port,pin) port |= (1<<pin)
#define set_input(port,pin) port &= ~(1<<pin)
#define set_output(port,pin) port |= (1<<pin)

#ifndef BAUD_TOL
#   define BAUD_TOL 2
#endif

// both buffers are static global vars in this file. interrupt handlers
// need to access them
static rx_ring_buffer rx_buffer;
static tx_ring_buffer tx_buffer;
// Local static variables
static uint8_t _msg_length, _data_length, _rx_queue, _address;

/**
 * Initialize the USART module with the BAUD rate predefined
 * in HardwareSerial.h
 *
 * This may implement passing addresses of registers, like the HardwareSerial
 * class from the Arduino library, but that's not necessary at this point.
 *
 */
NilRS485::NilRS485(rx_ring_buffer *rx_buffer_ptr, tx_ring_buffer *tx_buffer_ptr) {
 //   _rx_buffer = rx_buffer_ptr;
 //   _tx_buffer = tx_buffer_ptr;
}

/**
 * Initalize RS485
 * 
 * Set baudrate, 9N1, MPCM
 * enable receiver, transmitter ...
 *    
 */
void NilRS485::begin(unsigned long baud, uint8_t address) {
    set_output(DDRD, 4);   // Assign WriteEnable as output
    output_low(PORTD, 4);   // Disable WriteEnable on RS485 chip   
    _address = address;     // Assign address
    // Tick usec, 11 bits for one byte, _delay_loop_2 executes 4 CPU cycles
    // per iteration. The maximal possible delay is 262.14 ms / F_CPU in MHz.
    _tick = (F_CPU * 11) / (4 * baud);  
    
    // taken from <util/setbaud.h>
    uint8_t use2x = 0;
    uint16_t ubbr =  (F_CPU + 8UL * baud) / (16UL * baud) - 1UL;
    if ( (100 * (F_CPU)) > (16 * (ubbr + 1) * (100 * ubbr + ubbr * BAUD_TOL)) ) {
        use2x = 1;
        ubbr = (F_CPU + 4UL * baud) / (8UL * baud) - 1UL;
    }

    UBRR1L = ubbr & 0xff;
    UBRR1H = ubbr >> 8;
    if (use2x) {
        UCSR1A |= (1 << U2X1);
    } else {
        UCSR1A &= ~(1 << U2X1);
    }

    // Flush buffers
    tx_buffer.head = tx_buffer.tail = 0;
    rx_buffer.head = rx_buffer.tail = 0;
    _rx_queue = 0;
        
    UCSR1A |= (1<<MPCM1);  // Set MPCM
    UCSR1B = (1<<TXEN1) | (1<<RXEN1) | (1<<RXCIE1) | (1<<UCSZ12);
    // 
    // POZOR UCSRC = (1<<URSEL) pro nastaveni UCSRC 
    //
    UCSR1C = (1<<UCSZ11) | (1<<UCSZ10);
    
    // Bit          7 6 5 4 3 2 1 0
    //              RXCIEn TXCIEn UDRIEn RXENn TXENn UCSZn2 RXB8n TXB8n UCSRnB
    //              R/W R/W R/W R/W R/W R/W R R/W
    //Initial Value 0 0 0 0 0 0 0 0
//    UCSR0B |= (1<<TXEN0);  // Enable Transmitter
//    UCSR0B |= (1<<RXEN0);  // Enable Reciever
//    UCSR0B |= (1<<RXCIE0); // Enable Rx Complete Interrupt
//    UCSR0B |= (1<<UCSZ02); // 9 Bit
// UCSRC is set by default

}


/**
 * Receive handler
 */
NIL_IRQ_HANDLER(USART1_RX_vect) {
  
  /* On AVR this forces compiler to save registers r18-r31.*/
  NIL_IRQ_PROLOGUE();
  /* Nop on AVR.*/
  nilSysLockFromISR();
  
  if ((rx_buffer.head + 1) == rx_buffer.tail) { // full, do not receive anything 
    UCSR1A |= (1<<MPCM1); // set MPCM
    rx_buffer.head -= _msg_length; // return head
    return;
  }

  uint8_t status = UCSR1A; // Get status and 9th bit, then data from buffer
  uint8_t datah  = UCSR1B; 
  uint8_t datal  = UDR1;
  uint8_t _tmp;
  
  /* If error, return -1 */
  //if ( status & (1<<FE0)|(1<<DOR0)) output_high(PORTB, 5);
  //if ( status & (1<<FE0)) output_high(PORTB, 5);    
  // buffer overflow!
  
  datah = (datah >> RXB81) & 0x01; // Filter the 9th bit to 1 or 0
  // Check 9th bit
  if (datah){ // Address frame in middle tranmission = error or contention
    if (!(status & (1 << MPCM1))) { // MPCM0 bit is not set
      UCSR1A |= (1<<MPCM1); // set MPCM
      rx_buffer.head -= _msg_length; // return head
      return; // escape  
    } else {
      _tmp = datal & 0b00001111;
      // Our address or broadcast, we start receiveing data
      if ((_tmp == _address) || (_tmp == 15)) {
        UCSR1A &= ~(1<<MPCM1); // clear MPCM
        _msg_length = 1;
        _data_length = 0; 
      }          
      else { // not our address
        return; // escape 
      }
    }        
  } else { 
    _msg_length++;
    if (_msg_length == 2) { // control byte
      // We are receiveing data and MSG_CRC_SIZE
      // ACK NAK data is MSG_CRC_SIZE bytes
      _tmp = (datal >> 6);
      if (_tmp != FLAG_CMD) _data_length = (datal & 0b00111111) + MSG_CRC_SIZE;
      if (((_tmp == FLAG_ACK) || (_tmp == FLAG_NAK)) && ((datal & 0b00111111) != 0)) { // ACK NAK data length must be 0
        UCSR1A |= (1<<MPCM1); // set MPCM
        rx_buffer.head -= 1;  // return head, but last byte is not stored yet
        return; // escape  
      }           
    }
    if (_msg_length == 3) { // XOR check
      if ((rx_buffer.buffer[(uint8_t)(rx_buffer.head-2)] ^ rx_buffer.buffer[(uint8_t)(rx_buffer.head-1)]) != datal){ // error
        UCSR1A |= (1<<MPCM1); // set MPCM
        rx_buffer.head -= 2;  // return head, but last byte is not stored yet
        return; // escape 
      } 
    }
    if (_msg_length == MSG_HEADER_SIZE + _data_length) { // we have complete message
      _rx_queue++; 
      UCSR1A |= (1<<MPCM1); // set MPCM
      _msg_length = 0;             // set to 0 for future escapes 
      nilSemSignalI(&RS485NewMsg); // Signal handler thread.
    }  
  }
    
  rx_buffer.buffer[rx_buffer.head] = datal;  // save byte
  rx_buffer.head++;                          // advance head, watch out XOR !!!          
  
  /* Nop on AVR.*/
  nilSysUnlockFromISR();
  /* Epilogue performs rescheduling if required.*/
  NIL_IRQ_EPILOGUE();
}

/**
 * Read procedure
 */
uint8_t NilRS485::read(void) {
  return rx_buffer.buffer[rx_buffer.tail++];
} 

/**
 * Read message
 */
int8_t NilRS485::msg_read(RS485_msg *msg) {

  if (_rx_queue < 1) return 0; // no message
  _rx_queue--;
  
  uint8_t tmp_head;
  uint16_t tmp_crc = 0xFFFF; 
  
  // Read header
  tmp_head = read();
  msg->address     = tmp_head >> 4;  // from address

  tmp_head = read();
  msg->ctrl        = (tmp_head >> 6) & 0b11;
  msg->data_length = tmp_head & 0b111111;

  tmp_head = read(); // compare crc of header is done in interrupt
  // Read data and data crc, or ACK NAK signature 
  // ACK NAK signature has 2 bytes
  if (msg->ctrl != FLAG_CMD) {
    // Read data_length part
    for(tmp_head = 0; tmp_head < msg->data_length; tmp_head++) {
      msg->buffer[tmp_head] = read();
      tmp_crc = _crc16_update(tmp_crc, msg->buffer[tmp_head]);
    }
    // Read MSG_CRC_SIZE part
    for(tmp_head = 0; tmp_head < MSG_CRC_SIZE; tmp_head++) {
      msg->buffer[tmp_head + msg->data_length] = read();   
    }
  }

  // Compare data crc
  if (msg->ctrl == FLAG_DTA) {
    if (((tmp_crc>>8) != (uint8_t)msg->buffer[msg->data_length]) ||
       ((tmp_crc & 0b11111111) != (uint8_t)msg->buffer[msg->data_length+1])) return -1; // crc not valid
  }
  return 1;
}

/**
 * Data Register Empty Handler
 */
NIL_IRQ_HANDLER(USART1_UDRE_vect) {
  /* On AVR this forces compiler to save registers r18-r31.*/
  NIL_IRQ_PROLOGUE();
  /* Nop on AVR.*/
  nilSysLockFromISR();

  if (tx_buffer.head == tx_buffer.tail) {
    UCSR1B &= ~(1<<UDRIE1); // Buffer is empty, disable the interrupt
    tx_buffer.head = tx_buffer.tail = 0;
  } else {
    // Address or data  
    if (tx_buffer.tail == 0) {
      UCSR1B |= (1<<TXB81);  //set
    } else {
      UCSR1B &= ~(1<<TXB81); //clear
    }
    UDR1 = tx_buffer.buffer[tx_buffer.tail];
    tx_buffer.tail++;
  }
  
  /* Nop on AVR.*/
  nilSysUnlockFromISR();
  /* Epilogue performs rescheduling if required.*/
  NIL_IRQ_EPILOGUE();
}

/**
 * Transmit Complete Interrupt Handler
 *  
 * Automatically cleared when the interrupt is executed. 
 */
NIL_IRQ_HANDLER(USART1_TX_vect) {
  /* On AVR this forces compiler to save registers r18-r31.*/
  NIL_IRQ_PROLOGUE();
  /* Nop on AVR.*/
  nilSysLockFromISR();  
  
  output_low(PORTD, 4);    // Disable WriteEnable on RS485 chip 
  
  /* Nop on AVR.*/
  nilSysUnlockFromISR();
  /* Epilogue performs rescheduling if required.*/
  NIL_IRQ_EPILOGUE();  
}

void NilRS485::write(uint8_t data) {  
  // Advance the head, store the data    
  tx_buffer.buffer[tx_buffer.head] = data;
  tx_buffer.head++;
}


/**
 * Prepare and write message
 *   
 */
int8_t NilRS485::msg_write(RS485_msg *msg) {
  if (tx_buffer.head != tx_buffer.tail) return 0; // Transmitter is busy;
  
  uint8_t tmp_head1, tmp_head2;
  uint16_t tmp_crc = 0xFFFF;

  tx_buffer.head = tx_buffer.tail = 0; // Null buffer head and tail 
    
  // Write header
  tmp_head1 = _address << 4;               // Our address
  tmp_head1 = tmp_head1 + msg->address;    // To address
  write(tmp_head1);
  // Make sure that ACK NAK signature is only MSG_CRC_SIZE (2) bytes
  // Make sure that CMD data_length is 0 bytes
  tmp_head2 = msg->ctrl << 6;
  tmp_head2 |= msg->data_length;
  write(tmp_head2);
  write(tmp_head1 ^ tmp_head2);            // write XOR header
  
  // Write data 
  if (msg->ctrl != FLAG_CMD) {
    for(uint8_t i = 0; i < (msg->data_length); i++) {
      write(msg->buffer[i]);
      tmp_crc = _crc16_update(tmp_crc, msg->buffer[i]);   
    }
    if (msg->ctrl == FLAG_DTA) { // data CRC
      write(tmp_crc >> 8);
      write(tmp_crc & 0b11111111);
    } else { // ACK NAK signature 
      write(msg->buffer[0]);
      write(msg->buffer[1]);
    }
  }
  
  // wait for any incomming message to finish
  tmp_head1 = 0;
  while (!(UCSR1A & (1 << MPCM1))) { 
    _delay_loop_2(_tick); // delay 1 character 
    tmp_head1++;
    if ( tmp_head1 == LINE_READY_TIME_OUT) {
      tx_buffer.head = tx_buffer.tail = 0; // Null buffer head and tail
      return -1; // line is too busy, quit
      } 
  }
  // wait for line ready
  UCSR1B &= ~(1<<RXCIE1);  // Disable Rx Complete Interrupt
  UCSR1A &= ~(1<<MPCM1);   // Disable MPCM
  tmp_head1 = 0;
  do {
    tmp_head2 = UDR1; //just empty rx buffer 
    _delay_loop_2(_tick * ((random() % 15) + 1)); // random time, max is 16 nodes
    tmp_head1++;
    if ( tmp_head1 == LINE_READY_TIME_OUT) {
      tx_buffer.head = tx_buffer.tail = 0; // Null buffer head and tail
      UCSR1A |= (1<<MPCM1);  // Enable MPCM
      UCSR1B |= (1<<RXCIE1); // Enable Rx Complete Interrupt
      return -2; // line is too busy, quit
      } 
  } while (UCSR1A & (1<<RXC1)); // no char received, line is free
  
  UCSR1A |= (1<<MPCM1);   // Enable MPCM
  UCSR1B |= (1<<RXCIE1);  // Enable Rx Complete Interrupt
  // end of wait for line ready
  // Go for write
  output_high(PORTD, 4);  // Enable WriteEnable on RS485 chip                       
  UCSR1B |= (1<<UDRIE1);  // Enable Data Register Empty interrupt, start transmitting
  UCSR1B |= (1<<TXCIE1);  // Enable Transmit Complete interrupt (USART_TXC) 
  return 1;
}

/** Sleep while waiting for new message.
 * @note This function should not be used in the idle thread.
 * @return true if success or false if overrun or error.
 */
 bool nilWaitRS485NewMsg() {
  // Idle thread can't sleep.
 	if (nilIsIdleThread()) return false;
 	if (nilSemWaitTimeout(&RS485NewMsg, TIME_IMMEDIATE) != NIL_MSG_TMO) return false;
  nilSemWait(&RS485NewMsg);
  return true;
 }

NilRS485 RS485(&rx_buffer, &tx_buffer);