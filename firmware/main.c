/*-----------------------------------------------------------------------------
 * Code that turns a Cypress FX2 USB Controller into an USB JTAG adapter
 *-----------------------------------------------------------------------------
 * Copyright (C) 2005..2007 Kolja Waschk, ixo.de
 *-----------------------------------------------------------------------------
 * Check hardware.h/.c if it matches your hardware configuration (e.g. pinout).
 * Changes regarding USB identification should be made in product.inc!
 *-----------------------------------------------------------------------------
 * This code is part of usbjtag. usbjtag is free software; you can redistribute
 * it and/or modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version. usbjtag is distributed in the hope
 * that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.  You should have received a
 * copy of the GNU General Public License along with this program in the file
 * COPYING; if not, write to the Free Software Foundation, Inc., 51 Franklin
 * St, Fifth Floor, Boston, MA  02110-1301  USA
 *-----------------------------------------------------------------------------
 */
 
#include <isr.h>
#include <timer.h>
#include <delay.h>
#include <fx2regs.h>
#include <fx2utils.h>
#include <usb_common.h>
#include <usb_descriptors.h>
#include <usb_requests.h>
#include <syncdelay.h>
#include <i2c.h>
#include <sccb.h>
#include "config.h"

char capturing = 0;
char start = 0;
char stop = 0;

/* this interrupt turns the FIFOs on when the VSYNC is low (frame starts) */
/* TODO: this is not enough, the fifos need to be started only when a sync frame is received */
static void isr_ex0 (void) interrupt
{
  if (capturing == 0) {
    start = 1;
  }
  else {
    stop = 1;
    EX0 = 0; SYNCDELAY; // disable INT0 interrupt    
  }
  
  IE0 = 0; // clear flag
}

/* declarations */
void firmware_initialize(void);

void main(void)
{
  IT0 = 1; // INT0 on low-edge
  EX0 = 0; // disable INT0 interrupt
  IE0 = 0; // clear INT0 flag      
  hook_sv(SV_INT_0,(unsigned short) isr_ex0);
  
  /* initialize */
  EA = 0; // disable all interrupts
  firmware_initialize();
  setup_autovectors ();
  usb_install_handlers ();
  EA = 1; // enable interrupts
  
  
  /* renumerate */
  fx2_renumerate();

  /* main loop */
  while(1)
  {
    /* check for vendor commands */
    if (usb_setup_packet_avail())
      usb_handle_setup_packet();
      
    // insert main function here
    if (start == 1) {
      start = 0;
      //IOA |= bmBIT1; mdelay(1); // camera reset high (put camera out of reset)
      FIFORESET = 0x80; SYNCDELAY; // activate NAK-ALL to avoid race conditions
      FIFORESET = 0x02; SYNCDELAY; // Reset FIFO 2
      FIFORESET = 0x00; SYNCDELAY; //Release NAKALL
      EP2FIFOCFG &= ~bmWORDWIDE; SYNCDELAY;
      
      EP2FIFOBUF[0] = 0xF0; SYNCDELAY;
      EP2FIFOBUF[1] = 0xF0; SYNCDELAY;
      EP2FIFOBUF[2] = 0x0F; SYNCDELAY;
      EP2FIFOBUF[3] = 0x0F; SYNCDELAY;
      EP2BCH = 0; SYNCDELAY;
      EP2BCL = 4; SYNCDELAY;
      
      EP2FIFOCFG |= bmAUTOIN; SYNCDELAY; //switching to auto-in mode
      capturing = 1;
    }
    else if (stop == 1) {
      stop = 0;
      //IOA &= ~bmBIT1; mdelay(1); // camera reset high (put camera out of reset)
      while(!(EP2FIFOFLGS & 0x20));

      FIFORESET = 0x80; SYNCDELAY; // activate NAK-ALL to avoid race conditions
      FIFORESET = 0x02; SYNCDELAY; // Reset FIFO 2
      EP2FIFOCFG &= ~bmAUTOIN; SYNCDELAY; //switching to auto-in mode
      FIFORESET = 0x00; SYNCDELAY; //Release NAKALL
      
      EP2FIFOBUF[0] = 0xF0; SYNCDELAY;
      EP2FIFOBUF[1] = 0xF0; SYNCDELAY;
      EP2FIFOBUF[2] = 0x0F; SYNCDELAY;
      EP2FIFOBUF[3] = 0x0F; SYNCDELAY;
      EP2BCH = 0; SYNCDELAY;
      EP2BCL = 4; SYNCDELAY;
      
      //EP2FIFOCFG |= bmAUTOIN; SYNCDELAY; //switching to auto-in mode
      capturing = 0;

    }
  }
}


void firmware_initialize(void)
{
  // reset all fifos (so we start clean before turning the camera on)
  FIFORESET = 0x80; SYNCDELAY;					// From now on, NAK all, reset all FIFOS
  FIFORESET = 0x02; SYNCDELAY;					// Reset FIFO 2
  FIFORESET = 0x04; SYNCDELAY;					// Reset FIFO 4
  FIFORESET = 0x06; SYNCDELAY;					// Reset FIFO 6
  FIFORESET = 0x08; SYNCDELAY;					// Reset FIFO 8
  FIFORESET = 0x00; SYNCDELAY;					// Restore normal behaviour
  
  /* configure system */
  CPUCS |= bmCLKSPD0 /*| bmCLKINV*/; // set 24MHz clock + output clock (todo: test 48MHz)
  
  /* configure camera (needs to be done here so that the IFCLK is active before configuring FIFOs) */
  PORTACFG = bmINT0 /*| bmFLAGD*/; SYNCDELAY; // only PA0 as INT0 
  OEA |= bmBIT7 | bmBIT3 | bmBIT1; SYNCDELAY; // PA1 (camera reset) and PA3 (camera PWDN) as outputs, PA7 (led)
  IOA &= ~bmBIT3; mdelay(1); // put camera out of pwdn
  IOA |= bmBIT1; mdelay(1); // camera reset high (put camera out of reset)
  IOA &= ~bmBIT7; SYNCDELAY; // led off initially
  //PINFLAGSCD |= (0xc << 4); // EP2 as EF
  
  /* configure camera registers */
  sccb_modify(SCCB_COM3, bmSCALE_ENABLE, 0);
  sccb_modify(SCCB_COM7, bmQVGA | /*bmCOLORBAR |*/ bmRGB, 0);
  sccb_modify(SCCB_COM15, bmBIT4, 0); // RGB565
  //sccb_modify(SCCB_SCALING_XSC, bmTEST_PATTERN, 0);
  sccb_modify(SCCB_SCALING_YSC, bmTEST_PATTERN, 0);
  //sccb_modify(SCCB_COM17, bmDSPCOLORBAR, 0);
  
  /*// 12Mhz PCLK
  sccb_modify(SCCB_COM14, bmDCWSCALE | 0b10, 0); 
  sccb_modify(SCCB_SCALING_PCLK_DIV, bmDSPSCALE | 0b10, 0);*/
  
  //sccb_modify(SCCB_CLKRC, 1, bmNO_PRESCALE); // set clock prescale. 0 = CLKIN / 2, 1 = CLKIN = 4
  //sccb_modify
  mdelay(300); // wait 300ms for register settle
  
  /* configure fifos */
  //IFCONFIG |= bmIFCLKPOL; SYNCDELAY; // invert FIFO clock polarity, TODO: ok?
  IFCONFIG &= ~bmIFCLKSRC; SYNCDELAY; // disable int. FIFO clock (use ext. clock)
  IFCONFIG |= (bmIFCFG0 | bmIFCFG1); SYNCDELAY;// enable SLAVE FIFO operation
  
  // enable FIFO configuration
  REVCTL = 3; SYNCDELAY;
  
  // disable unused endpoints
  EP1OUTCFG &= ~bmVALID; SYNCDELAY;
  EP1INCFG  &= ~bmVALID; SYNCDELAY;
  EP6CFG &= ~bmVALID; SYNCDELAY;
  EP4CFG &= ~bmVALID; SYNCDELAY;
  EP8CFG &= ~bmVALID; SYNCDELAY;
  
  // enable EP2
  EP2CFG = bmVALID | bmISOCHRONOUS | bmIN | bmQUADBUF | bm1KBUF; SYNCDELAY;
  
  // setup EP2
  EP2FIFOCFG &= ~bmWORDWIDE; SYNCDELAY; // set EP2 8bits (PORTB -> FD[7:0]) 
  
  //EP2ISOINPKTS |= bmBIT1 | bmBIT0; // set INPPF[1:0] = 3 -> 3 packets per microframe
  // TODO: medir que realmente este teniendo 320x240
  EP2AUTOINLENH = (626 >> 8); SYNCDELAY;
  EP2AUTOINLENL = (626 & 0xFF); SYNCDELAY;
  FIFOINPOLAR |= bmBIT2; SYNCDELAY; // set SLWR active-high
  
  // disable access to FIFOs
  REVCTL = 0; SYNCDELAY;  
    
  // TODO: ver como setear las cosas para que se empiece a hacer todo recien al inicio de un frame (falling edge de VSYNC, por ej)
  // TODO: set FIFOADDR pins!
  // TODO: PKTEND to send a line directly?  
}

unsigned char app_vendor_cmd(void)
{
  // OUT requests. Pretend we handle them all...
  // TODO: what is this?
  if ((bRequestType & bmRT_DIR_MASK) == bmRT_DIR_OUT){
    return 1;
  }

  // IN requests.    
  switch (bRequest){
    // get firmware version / git revision
    case 0x94: 
    case 0x95:
    {
      int i = 0;
      const char* ver = (bRequest == 0x94 ? FIRMWARE_VERSION : FIRMWARE_GIT_REVISION);
      while (ver[i] != '\0') {
        EP0BUF[i] = ver[i];
        i++;
      }
      EP0BCH = 0; // Arm endpoint
      EP0BCL = i;
    }
    break;
    case 0x96:
    {
      EX0 = 1; // enable INT0 interrupt
    }
    break;
    default:
      // dummy data
      EP0BUF[0] = 0x36;
      EP0BUF[1] = 0x83;
      EP0BCH = 0;
      EP0BCL = (wLengthL<2) ? wLengthL : 2;
    break;
  }

  return 1;
}




