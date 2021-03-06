//*****************************************************************************
//
// enet_io.c - I/O control via a web server.
//
// Copyright (c) 2013-2016 Texas Instruments Incorporated.  All rights reserved.
// Software License Agreement
// 
// Texas Instruments (TI) is supplying this software for use solely and
// exclusively on TI's microcontroller products. The software is owned by
// TI and/or its suppliers, and is protected under applicable copyright
// laws. You may not combine this software with "viral" open-source
// software in order to form a larger program.
// 
// THIS SOFTWARE IS PROVIDED "AS IS" AND WITH ALL FAULTS.
// NO WARRANTIES, WHETHER EXPRESS, IMPLIED OR STATUTORY, INCLUDING, BUT
// NOT LIMITED TO, IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE APPLY TO THIS SOFTWARE. TI SHALL NOT, UNDER ANY
// CIRCUMSTANCES, BE LIABLE FOR SPECIAL, INCIDENTAL, OR CONSEQUENTIAL
// DAMAGES, FOR ANY REASON WHATSOEVER.
// 
// This is part of revision 2.1.3.156 of the EK-TM4C1294XL Firmware Package.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* hw headers */
#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_nvic.h"
#include "inc/hw_types.h"
#include "drivers/pinout.h"
#include "drivers/buttons.h"


/* Peripherial drivers */
#include "driverlib/debug.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/pin_map.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/sysctl.h"
#include "driverlib/flash.h"
#include "driverlib/sysctl.h"
#include "driverlib/uart.h"
#include "driverlib/systick.h"
#include "driverlib/timer.h"

/* Utils */
#include "utils/uartstdio.h"
#include "utils/locator.h"
#include "utils/lwiplib.h"
#include "utils/ustdlib.h"
#include "utils/locator.h"
#include "utils/lwiplib.h"
#include "utils/uartstdio.h"
#include "utils/ustdlib.h"

/* misc */
#include "httpserver_raw/httpd.h"
#include "io.h"
#include "cgifuncs.h"

/* sensor libraries */
/* I2C driver lib */
#include "sensorlib/i2cm_drv.h"

/* TMP006 - temperature */
#include "sensorlib/hw_tmp006.h"
#include "sensorlib/tmp006.h"

/* SHT21 - humidity */
#include "sensorlib/sht21.h"
#include "sensorlib/hw_sht21.h"

/* BMP180 - pressure */
#include "sensorlib/hw_bmp180.h"
#include "sensorlib/bmp180.h"

/* ISL29023 - light */
#include "sensorlib/isl29023.h"
#include "sensorlib/hw_isl29023.h"

#include "weather_station/weather_station.h"

//*****************************************************************************
//
// Defines for setting up the system clock.
//
//*****************************************************************************
#define SYSTICKHZ               100
#define SYSTICKMS               (1000 / SYSTICKHZ)

//*****************************************************************************
//
// Interrupt priority definitions.  The top 3 bits of these values are
// significant with lower values indicating higher priority interrupts.
//
//*****************************************************************************
#define SYSTICK_INT_PRIORITY    0x80		/* Systick INT priority is the highest */
#define ETHERNET_INT_PRIORITY   0xC0		/* ETH priority */
#define GPIOH_INT_PRIORITY		0xE0		/* Light threshold INT priority is the lowest */
#define WS_REFRESH_PERIOD_MS	50			/* Refreshing period of sensor data */

//*****************************************************************************
//
// A set of flags.  The flag bits are defined as follows:
//
//     0 -> An indicator that the animation timer interrupt has occurred.
//
//*****************************************************************************
#define FLAG_TICK            0
static volatile unsigned long g_ulFlags;
static volatile bool ipSetupRdy;

//*****************************************************************************
//
// External Application references.
//
//*****************************************************************************
extern void httpd_init(void);

//*****************************************************************************
//
// SSI tag indices for each entry in the g_pcSSITags array.
//
//*****************************************************************************
#define SSI_INDEX_LEDSTATE  0
#define SSI_INDEX_FORMVARS  1
#define SSI_INDEX_SPEED     2

//*****************************************************************************
//
// This array holds all the strings that are to be recognized as SSI tag
// names by the HTTPD server.  The server will call SSIHandler to request a
// replacement string whenever the pattern <!--#tagname--> (where tagname
// appears in the following array) is found in ".ssi", ".shtml" or ".shtm"
// files that it serves.
//
//*****************************************************************************
static const char *g_pcConfigSSITags[] =
{
    "LEDtxt",        // SSI_INDEX_LEDSTATE
    "FormVars",      // SSI_INDEX_FORMVARS
    "speed"          // SSI_INDEX_SPEED
};

//*****************************************************************************
//
// The number of individual SSI tags that the HTTPD server can expect to
// find in our configuration pages.
//
//*****************************************************************************
#define NUM_CONFIG_SSI_TAGS     (sizeof(g_pcConfigSSITags) / sizeof (char *))

//*****************************************************************************
//
// Prototypes for the various CGI handler functions.
//
//*****************************************************************************
static char *ControlCGIHandler(int32_t iIndex, int32_t i32NumParams,
                               char *pcParam[], char *pcValue[]);
static char *SetTextCGIHandler(int32_t iIndex, int32_t i32NumParams,
                               char *pcParam[], char *pcValue[]);

//*****************************************************************************
//
// Prototype for the main handler used to process server-side-includes for the
// application's web-based configuration screens.
//
//*****************************************************************************
static int32_t SSIHandler(int32_t iIndex, char *pcInsert, int32_t iInsertLen);

//*****************************************************************************
//
// CGI URI indices for each entry in the g_psConfigCGIURIs array.
//
//*****************************************************************************
#define CGI_INDEX_CONTROL       0
#define CGI_INDEX_TEXT          1

//*****************************************************************************
//
// This array is passed to the HTTPD server to inform it of special URIs
// that are treated as common gateway interface (CGI) scripts.  Each URI name
// is defined along with a pointer to the function which is to be called to
// process it.
//
//*****************************************************************************
static const tCGI g_psConfigCGIURIs[] =
{
    { "/iocontrol.cgi", (tCGIHandler)ControlCGIHandler }, // CGI_INDEX_CONTROL
    { "/settxt.cgi", (tCGIHandler)SetTextCGIHandler }     // CGI_INDEX_TEXT
};

//*****************************************************************************
//
// The number of individual CGI URIs that are configured for this system.
//
//*****************************************************************************
#define NUM_CONFIG_CGI_URIS     (sizeof(g_psConfigCGIURIs) / sizeof(tCGI))

//*****************************************************************************
//
// The file sent back to the browser by default following completion of any
// of our CGI handlers.  Each individual handler returns the URI of the page
// to load in response to it being called.
//
//*****************************************************************************
#define DEFAULT_CGI_RESPONSE    "/io_cgi.ssi"

//*****************************************************************************
//
// The file sent back to the browser in cases where a parameter error is
// detected by one of the CGI handlers.  This should only happen if someone
// tries to access the CGI directly via the broswer command line and doesn't
// enter all the required parameters alongside the URI.
//
//*****************************************************************************
#define PARAM_ERROR_RESPONSE    "/perror.htm"

#define JAVASCRIPT_HEADER                                                     \
    "<script type='text/javascript' language='JavaScript'><!--\n"
#define JAVASCRIPT_FOOTER                                                     \
    "//--></script>\n"

//*****************************************************************************
//
// Timeout for DHCP address request (in seconds).
//
//*****************************************************************************
#ifndef DHCP_EXPIRE_TIMER_SECS
#define DHCP_EXPIRE_TIMER_SECS  45
#endif
/* Extern weather station variables */
bool TempDataFlag;
bool HumidityDataFlag;
bool PressureDataFlag;
bool LightDataFlag;
float TempAmbientMeas, TempObjectMeas;
float HumidityMeas;
float PressureMeas;
float LightMeas;
uint8_t LightMask;
int SystickCounter = 0;

/* Extern IO variables */
int32_t tempInteger, tempFraction;
int32_t humidityInteger, humidityFraction;
int32_t pressureInteger, pressureFraction;
int32_t lightInteger, lightFraction;

//*****************************************************************************
//
// The current IP address.
//
//*****************************************************************************
uint32_t g_ui32IPAddress;

//*****************************************************************************
//
// The system clock frequency.  Used by the SD card driver.
//
//*****************************************************************************
uint32_t g_ui32SysClock;


//*****************************************************************************
//
// The error routine that is called if the driver library encounters an error.
//
//*****************************************************************************
#ifdef DEBUG
void
__error__(char *pcFilename, uint32_t ui32Line)
{
}
#endif


//*****************************************************************************
//
// This CGI handler is called whenever the web browser requests iocontrol.cgi.
//
//*****************************************************************************
static char *
ControlCGIHandler(int32_t iIndex, int32_t i32NumParams, char *pcParam[],
                  char *pcValue[])
{
    int32_t i32LEDState, i32Speed;
    bool bParamError;

    //
    // We have not encountered any parameter errors yet.
    //
    bParamError = false;

    //
    // Get each of the expected parameters.
    //
    i32LEDState = FindCGIParameter("LEDOn", pcParam, i32NumParams);
    i32Speed = GetCGIParam("speed_percent", pcParam, pcValue, i32NumParams,
            &bParamError);

    //
    // Was there any error reported by the parameter parser?
    //
    if(bParamError || (i32Speed < 0) || (i32Speed > 100))
    {
        return(PARAM_ERROR_RESPONSE);
    }

    //
    // We got all the parameters and the values were within the expected ranges
    // so go ahead and make the changes.
    //
    io_set_led((i32LEDState == -1) ? false : true);
    io_set_animation_speed(i32Speed);

    //
    // Send back the default response page.
    //
    return(DEFAULT_CGI_RESPONSE);
}

//*****************************************************************************
//
// This CGI handler is called whenever the web browser requests settxt.cgi.
//
//*****************************************************************************
static char *
SetTextCGIHandler(int32_t i32Index, int32_t i32NumParams, char *pcParam[],
                  char *pcValue[])
{
    long lStringParam;
    char pcDecodedString[48];

    //
    // Find the parameter that has the string we need to display.
    //
    lStringParam = FindCGIParameter("DispText", pcParam, i32NumParams);

    //
    // If the parameter was not found, show the error page.
    //
    if(lStringParam == -1)
    {
        return(PARAM_ERROR_RESPONSE);
    }

    //
    // The parameter is present. We need to decode the text for display.
    //
    DecodeFormString(pcValue[lStringParam], pcDecodedString, 48);

    //
    // Print sting over the UART
    //
    UARTprintf(pcDecodedString);
    UARTprintf("\n");

    //
    // Tell the HTTPD server which file to send back to the client.
    //
    return(DEFAULT_CGI_RESPONSE);
}

//*****************************************************************************
//
// This function is called by the HTTP server whenever it encounters an SSI
// tag in a web page.  The iIndex parameter provides the index of the tag in
// the g_pcConfigSSITags array. This function writes the substitution text
// into the pcInsert array, writing no more than iInsertLen characters.
//
//*****************************************************************************
static int32_t
SSIHandler(int32_t iIndex, char *pcInsert, int32_t iInsertLen)
{
    //
    // Which SSI tag have we been passed?
    //
    switch(iIndex)
    {
        case SSI_INDEX_LEDSTATE:
            io_get_ledstate(pcInsert, iInsertLen);
            break;

        case SSI_INDEX_FORMVARS:
            usnprintf(pcInsert, iInsertLen,
                    "%sls=%d;\nsp=%d;\n%s",
                    JAVASCRIPT_HEADER,
                    io_is_led_on(),
                    io_get_animation_speed(),
                    JAVASCRIPT_FOOTER);
            break;

        case SSI_INDEX_SPEED:
            io_get_animation_speed_string(pcInsert, iInsertLen);
            break;

        default:
            usnprintf(pcInsert, iInsertLen, "??");
            break;
    }

    //
    // Tell the server how many characters our insert string contains.
    //
    return(strlen(pcInsert));
}

//*****************************************************************************
//
// The interrupt handler for the SysTick interrupt.
//
//*****************************************************************************
void
SysTickIntHandler(void)
{
    //
    // Call the lwIP timer handler.
    //
    lwIPTimer(SYSTICKMS);
    if(ipSetupRdy)
    {
		/* Increment counter for sensors */
		if( !(SystickCounter % ((SYSTICKHZ * WS_REFRESH_PERIOD_MS)/1000) ) )
		{
		tempInteger = IntegerPart(TempAmbientMeas);
		tempFraction = FractionPart(TempAmbientMeas);

		humidityInteger = IntegerPart(HumidityMeas);
		humidityFraction = FractionPart(HumidityMeas);

		pressureInteger = IntegerPart(PressureMeas);
		pressureFraction = FractionPart(PressureMeas);

		lightInteger = IntegerPart(LightMeas);
		lightFraction = FractionPart(LightMeas);

		UARTprintf("Temperature: %d.%d,  Humidity: %d.%d,  Pressure: %d.%d, Light: %d.%d\n ", tempInteger, tempFraction,
	    																					  humidityInteger, humidityFraction,
	    																					  pressureInteger, pressureFraction,
	    																					  lightInteger, lightFraction);
			/* Clear data ready flags */
			TempDataFlag = false;
			HumidityDataFlag = false;
			PressureDataFlag = false;
			LightDataFlag = false;
			SystickCounter = 0;
		}
		SystickCounter++;
    }
}

//*****************************************************************************
//
// The interrupt handler for the timer used to pace the animation.
//
//*****************************************************************************
void
AnimTimerIntHandler(void)
{
    //
    // Clear the timer interrupt.
    //
    MAP_TimerIntClear(TIMER2_BASE, TIMER_TIMA_TIMEOUT);

    //
    // Indicate that a timer interrupt has occurred.
    //
    HWREGBITW(&g_ulFlags, FLAG_TICK) = 1;
}

//*****************************************************************************
//
// Display an lwIP type IP Address.
//
//*****************************************************************************
void
DisplayIPAddress(uint32_t ui32Addr)
{
    char pcBuf[16];

    //
    // Convert the IP Address into a string.
    //
    usprintf(pcBuf, "%d.%d.%d.%d", ui32Addr & 0xff, (ui32Addr >> 8) & 0xff,
            (ui32Addr >> 16) & 0xff, (ui32Addr >> 24) & 0xff);

    //
    // Display the string.
    //
    UARTprintf(pcBuf);
}

//*****************************************************************************
//
// Required by lwIP library to support any host-related timer functions.
//
//*****************************************************************************
void
lwIPHostTimerHandler(void)
{
    uint32_t ui32NewIPAddress;

    //
    // Get the current IP address.
    //
    ui32NewIPAddress = lwIPLocalIPAddrGet();

    //
    // See if the IP address has changed.
    //
    if(ui32NewIPAddress != g_ui32IPAddress)
    {
        //
        // See if there is an IP address assigned.
        //
        if(ui32NewIPAddress == 0xffffffff)
        {
            //
            // Indicate that there is no link.
            //
            UARTprintf("Waiting for link.\n");
        }
        else if(ui32NewIPAddress == 0)
        {
            //
            // There is no IP address, so indicate that the DHCP process is
            // running.
            //
            UARTprintf("Waiting for IP address.\n");
        }
        else
        {
            //
            // Display the new IP address.
            //
            UARTprintf("IP Address: ");
            DisplayIPAddress(ui32NewIPAddress);
            UARTprintf("\n");
            UARTprintf("Open a browser and enter the IP address.\n");
            UARTprintf("Measurements will be sent periodically every %d milliseconds\n", WS_REFRESH_PERIOD_MS);
            ipSetupRdy = true;
        }

        //
        // Save the new IP address.
        //
        g_ui32IPAddress = ui32NewIPAddress;
    }

    //
    // If there is not an IP address.
    //
    if((ui32NewIPAddress == 0) || (ui32NewIPAddress == 0xffffffff))
    {
       //
       // Do nothing and keep waiting.
       //
    }
}

//*****************************************************************************
//
// This example demonstrates the use of the Ethernet Controller and lwIP
// TCP/IP stack to control various peripherals on the board via a web
// browser.
//
//*****************************************************************************
int
main(void)
{
    uint32_t ui32User0, ui32User1;
    uint8_t pui8MACArray[8];

    //
    // Make sure the main oscillator is enabled because this is required by
    // the PHY.  The system must have a 25MHz crystal attached to the OSC
    // pins.  The SYSCTL_MOSC_HIGHFREQ parameter is used when the crystal
    // frequency is 10MHz or higher.
    //
    SysCtlMOSCConfigSet(SYSCTL_MOSC_HIGHFREQ);

    //
    // Run from the PLL at 120 MHz.
    //
    g_ui32SysClock = MAP_SysCtlClockFreqSet((SYSCTL_XTAL_25MHZ |
                                             SYSCTL_OSC_MAIN |
                                             SYSCTL_USE_PLL |
                                             SYSCTL_CFG_VCO_480), 120000000);

    //
    // Configure the device pins.
    //
    PinoutSet(true, false);

    //
    // Configure debug port for internal use.
    //
    UARTStdioConfig(0, 115200, g_ui32SysClock);

    //
    // Clear the terminal and print a banner.
    //
    UARTprintf("Weather station serial test application\n");

    /* Enable the I2C7 peripheral before use. */
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOD);
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_I2C7);

    /* Configure the pin muxing for I2C7 functions on port D0 and D1. */
    ROM_GPIOPinConfigure(GPIO_PD0_I2C7SCL);
    ROM_GPIOPinConfigure(GPIO_PD1_I2C7SDA);

    /* Select the I2C function for these pins.  This function will also configure the GPIO pins pins for I2C operation, setting them to
     * open-drain operation with weak pull-ups. */
    GPIOPinTypeI2CSCL(GPIO_PORTD_BASE, GPIO_PIN_0);
    ROM_GPIOPinTypeI2C(GPIO_PORTD_BASE, GPIO_PIN_1);

    /* Configure and Enable the GPIO interrupt. Used for DRDY from the TMP006 and for INT signal from the ISL29023*/
    ROM_GPIOPinTypeGPIOInput(GPIO_PORTH_BASE, GPIO_PIN_2);
    ROM_GPIOPinTypeGPIOInput(GPIO_PORTE_BASE, GPIO_PIN_5);
    GPIOIntEnable(GPIO_PORTH_BASE, GPIO_PIN_2);
    GPIOIntEnable(GPIO_PORTE_BASE, GPIO_PIN_5);
    ROM_GPIOIntTypeSet(GPIO_PORTH_BASE, GPIO_PIN_2, GPIO_FALLING_EDGE);
    ROM_GPIOIntTypeSet(GPIO_PORTE_BASE, GPIO_PIN_5, GPIO_FALLING_EDGE);
    ROM_IntEnable(INT_GPIOH);
    ROM_IntEnable(INT_GPIOE);

    /* Enable interrupts to the processor. */
    ROM_IntMasterEnable();

    /* Initialize I2C peripheral. */
    initI2C();

    /* Initialize the TMP006 */
    tempSensorInit();

    /* Initialize the SHT21. */
    humiditySensorInit();

    /* Initialize the BMP180. */
    pressureSensorInit();

    /* Initialize ISL29023 */
    lightSensorInit();

    //
    // Configure SysTick for a periodic interrupt.
    //
    ipSetupRdy = false;
    MAP_SysTickPeriodSet(g_ui32SysClock / SYSTICKHZ);
    MAP_SysTickEnable();
    MAP_SysTickIntEnable();

    //
    // Configure the hardware MAC address for Ethernet Controller filtering of
    // incoming packets.  The MAC address will be stored in the non-volatile
    // USER0 and USER1 registers.
    //
    MAP_FlashUserGet(&ui32User0, &ui32User1);
    if((ui32User0 == 0xffffffff) || (ui32User1 == 0xffffffff))
    {
        //
        // Let the user know there is no MAC address
        //
        UARTprintf("No MAC programmed!\n");

        while(1)
        {
        }
    }

    //
    // Tell the user what we are doing just now.
    //
    UARTprintf("Waiting for IP.\n");

    //
    // Convert the 24/24 split MAC address from NV ram into a 32/16 split
    // MAC address needed to program the hardware registers, then program
    // the MAC address into the Ethernet Controller registers.
    //
    pui8MACArray[0] = ((ui32User0 >>  0) & 0xff);
    pui8MACArray[1] = ((ui32User0 >>  8) & 0xff);
    pui8MACArray[2] = ((ui32User0 >> 16) & 0xff);
    pui8MACArray[3] = ((ui32User1 >>  0) & 0xff);
    pui8MACArray[4] = ((ui32User1 >>  8) & 0xff);
    pui8MACArray[5] = ((ui32User1 >> 16) & 0xff);

    //
    // Initialze the lwIP library, using DHCP.
    //
    lwIPInit(g_ui32SysClock, pui8MACArray, 0, 0, 0, IPADDR_USE_DHCP);

    //
    // Setup the device locator service.
    //
    LocatorInit();
    LocatorMACAddrSet(pui8MACArray);
    LocatorAppTitleSet("EK-TM4C1294XL enet_io");

    //
    // Initialize a sample httpd server.
    //
    httpd_init();

    //
    // Set the interrupt priorities.  We set the SysTick interrupt to a higher
    // priority than the Ethernet interrupt to ensure that the file system
    // tick is processed if SysTick occurs while the Ethernet handler is being
    // processed.  This is very likely since all the TCP/IP and HTTP work is
    // done in the context of the Ethernet interrupt.
    //
    MAP_IntPrioritySet(INT_EMAC0, ETHERNET_INT_PRIORITY);
    MAP_IntPrioritySet(FAULT_SYSTICK, SYSTICK_INT_PRIORITY);

    //
    // Pass our tag information to the HTTP server.
    //
    http_set_ssi_handler((tSSIHandler)SSIHandler, g_pcConfigSSITags,
            NUM_CONFIG_SSI_TAGS);

    //
    // Pass our CGI handlers to the HTTP server.
    //
    http_set_cgi_handlers(g_psConfigCGIURIs, NUM_CONFIG_CGI_URIS);

    //
    // Initialize IO controls
    //
    io_init();
    //
    // Loop forever, processing the on-screen animation.  All other work is
    // done in the interrupt handlers.
    //
    while(1)
    {

        /* Run until every measurement data is ready */
        if( (TempDataFlag == 0) || (HumidityDataFlag == 0) ||
            		(PressureDataFlag == 0) || (LightDataFlag == 0) )
        {
        	/* TMP006 sensor */
        	measureTemp();

        	/* SHT21 sensor */
        	measureHumidity();

          	/* BMP180 sensor */
          	measurePressure();

            /*ISL29023 sensor */
            measureLight();
        }
        else
        {
        	MAP_SysCtlSleep();
        }

    }
}
