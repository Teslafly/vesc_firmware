/*
	Copyright 2016 - 2022 Benjamin Vedder	benjamin@vedder.se
	Copyright 2022 Marcos Chaparro	mchaparro@powerdesigns.ca
	Copyright 2022 Jakub Tomczak
	Copyright 2022 Marshall Scholz

	This file is part of the VESC firmware.

	The VESC firmware is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    The VESC firmware is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.


	Encoder driver for TLE5012
	https://www.infineon.com/dgdl/Infineon-TLE5012B_Exxxx-DataSheet-v02_01-EN.pdf?fileId=db3a304334fac4c601350f31c43c433f
 */

#include "enc_mt6816.h"

#include "ch.h"
#include "hal.h"
#include "stm32f4xx_conf.h"
#include "hw.h"
#include "mc_interface.h"
#include "utils.h"
#include "spi_bb.h"
#include "timer.h"

#include <math.h>
#include <string.h>

// Bitmasks for several read and write functions
#define TLE5012_SYSTEM_ERROR_MASK           0x4000    //!< \brief System error masks for safety words
#define TLE5012_INTERFACE_ERROR_MASK        0x2000    //!< \brief Interface error masks for safety words
#define TLE5012_INV_ANGLE_ERROR_MASK        0x1000    //!< \brief Angle error masks for safety words

#define TLE5012_CRC_POLYNOMIAL              0x1D      //!< \brief values used for calculating the CRC
#define TLE5012_CRC_SEED                    0xFF
// #define TLE5012_CRC_NUM_REGISTERS           0x0008    //!< \brief number of CRC relevant registers
// #define TLE5012_MAX_REGISTER_MEM            0x0030    //!< \brief max readable register values buffer
// #define TLE5012_MAX_NUM_REG                 0x16      //!< \brief defines the value for temporary data to read all readable registers


enum tle5012_registers {
	REG_STAT    =  0,     //!< STAT status register
	REG_ACSTAT  =  1,     //!< ACSTAT activation status register
	REG_AVAL    =  2,     //!< AVAL angle value register
	REG_ASPD    =  3,     //!< ASPD angle speed register
	REG_AREV    =  4,     //!< AREV angle revolution register
	REG_FSYNC   =  5,     //!< FSYNC frame synchronization register
	REG_MOD_1   =  6,     //!< MOD_1 interface mode1 register
	REG_SIL     =  7,     //!< SIL register
	REG_MOD_2   =  8,     //!< MOD_2 interface mode2 register
	REG_MOD_3   =  9,     //!< MOD_3 interface mode3 register
	REG_OFFX    = 10,     //!< OFFX offset x
	REG_OFFY    = 11,     //!< OFFY offset y
	REG_SYNCH   = 12,     //!< SYNCH synchronicity
	REG_IFAB    = 13,     //!< IFAB register
	REG_MOD_4   = 14,     //!< MOD_4 interface mode4 register
	REG_TCO_Y   = 15,     //!< TCO_Y temperature coefficient register
	REG_ADC_X   = 16,     //!< ADC_X ADC X-raw value
	REG_ADC_Y   = 17,     //!< ADC_Y ADC Y-raw value
	REG_D_MAG   = 18,     //!< D_MAG angle vector magnitude
	REG_T_RAW   = 19,     //!< T_RAW temperature sensor raw-value
	REG_IIF_CNT = 20,     //!< IIF_CNT IIF counter value
	REG_T25O    = 21,     //!< T25O temperature 25°c offset value
};


typedef enum spi_direction {
	READ = true, 
	WRITE = false
} spi_direction; 

uint8_t enc_tle5012_transfer(TLE5012_config_t *cfg, uint8_t address, uint16_t *data, spi_direction read, bool safe);

uint8_t crc8(uint8_t *data, uint8_t length);
uint8_t checkSafety(uint16_t command, uint16_t safetyword, const uint16_t *readreg, uint16_t length);


bool enc_tle5012_init(TLE5012_config_t *cfg) {
	bool hwspi = false;
	if (hwspi){
		// if (cfg->spi_dev == NULL) {
		// 	return;
		// }

		// ssc mode uses mosi pin only. 
		// palSetPadMode(cfg->sck_gpio, cfg->sck_pin, PAL_MODE_ALTERNATE(6) | PAL_STM32_OSPEED_HIGHEST);
		// // palSetPadMode(cfg->miso_gpio, cfg->miso_pin, PAL_MODE_ALTERNATE(6) | PAL_STM32_OSPEED_HIGHEST); // not required for ssc
		// palSetPadMode(cfg->nss_gpio, cfg->nss_pin, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_HIGHEST);
		// palSetPadMode(cfg->mosi_gpio, cfg->mosi_pin, PAL_MODE_ALTERNATE(6) | PAL_STM32_OSPEED_HIGHEST);

		// spiStart(cfg->spi_dev, &(cfg->hw_spi_cfg));
		return true;

	} else {
		// software ssc
		memset(&cfg->state, 0, sizeof(TLE5012_state)); 

		// ssc mode uses mosi pin only. (both MISO/MOSI set to MOSI gpio)
		spi_bb_init(&(cfg->sw_spi));

		cfg->state.last_status_error = 0;
		cfg->state.spi_error_rate = 0.0;
		cfg->state.encoder_no_magnet_error_rate = 0.0;

		return true;
	}


	/*
	// TLE5012B E1000 (IIF) configuration:
	IIF-type: E1000
	The TLE5012B E1000 is preconfigured for Incremental Interface and fast angle update rate (42.7 μs). 
	It is most suitable for BLDC motor commutation.
	• Incremental Interface A/B mode.
	• 12bit mode, one count per 0.088° angle step.
	• Absolute count enabled.
	• Autocalibration mode 1 enabled.
	• Prediction disabled.
	• Hysteresis set to 0.703°.
	• IFA/IFB/IFC pins set to push-pull output.
	• SSC interface’s DATA pin set to push-pull output.
	• IFA/IFB/IFC pins set to strong driver, DATA pin set to strong driver, fast edge.
	• Voltage spike filter on input pads disabled.

	- Interface Mode1 Register
		fir_md = 0b01  // Update Rate Setting (Filter Decimation), 42.7 μs (minimum)
		clk_sel = 0    // Clock Source Select, internal
		dspu_hold = 0  // DSPU in normal schedule operation, no watchdog reset
		iif_mod = 0b01 // Incremental Interface Mode, A/B operation with Index on IFC pin
		Offset = 0x06
		mask = 0b11 00000000 1 0 1 11 // reserved unset only
		word = 0b01 00000000 0 0 0 01
	- Interface Mode2 Register
		predict: 0     // prediction disabled
		autocal = 0b01 //auto-cal. mode 1: update every angle update cycle
			           // change autocal to 0b00? (no auto-calibration)
		Offset = 0x08
		mask = 0b0 1111111111 1 1 11
		word = 0b0 1111111111 0 0 01
	- Interface Mode3 Register
		spikef = 0   // Analog Spike Filter of Input Pads, disabled
		ssc_od = 0   // SSC-Interface Data Pin Output Mode, Push-Pull
		PAD_drv = 00 // Configuration of Pad-Driver, 
					 // IFA/IFB/IFC: strong driver, DATA: strong driver, fast edge
		Offset = 0x09
		mask = 0b00000000000 1 1 11 // can overwrite ang base to 0?
		word = 0b00000000000 0 0 00
	- TCO_Y // nothing derivate-specific
		sbist = 1// built in self test on startup
		crc = crc of parameters if autocalibrate not set.
	- IFAB Register
		fir_udr = 1 // FIR Update Rate, FIR_MD = ‘01’ (42.7 μs)
		ifab_od = 0 // IFA,IFB,IFC Output Mode, Push-Pull
		ifab_hyst = 0b11 // HSM and IIF Mode: Hysteresis, 0.70° (max)
		mask = 0b00000000000 1 1 11
		word = 0b00000000000 1 0 11
	- Interface Mode4 Register
		hsm_plp = 0b0001 // Incremental Interface Mode: Absolute Count, absolute count enabled
		ifab_res = 0b00  // Incremental Interface Mode: IIF resolution, 12bit, 0.088° step
		if_md = 0b00     // Interface Mode on IFA,IFB,IFC, IIF
		mask = 0b0000000 1111 11 11
		word = 0b0000000 0001 00 00
	*/

	// set up control registers to be identical across tle5012 variants using above settings
	uint16_t tleregister;
	// Interface Mode1
	enc_tle5012_transfer(cfg, 0x06, &tleregister, READ, true);
	tleregister = tleregister & ~0b110000000010111; // mask (1 = cleared)
	tleregister = tleregister |  0b010000000000001; // set bits
	enc_tle5012_transfer(cfg, 0x06, &tleregister, WRITE, true);

	// Interface Mode2
	enc_tle5012_transfer(cfg, 0x08, &tleregister, READ, true);
	tleregister = tleregister & ~0b011111111111111;
	tleregister = tleregister |  0b011111111110001;
	enc_tle5012_transfer(cfg, 0x08, &tleregister, WRITE, true);

	// Interface Mode3
	enc_tle5012_transfer(cfg, 0x09, &tleregister, READ, true);
	tleregister = tleregister & ~0b000000000001111;
	tleregister = tleregister |  0b000000000000000;
	enc_tle5012_transfer(cfg, 0x09, &tleregister, WRITE, true);

	// IFAB Register
	enc_tle5012_transfer(cfg, 0x0D, &tleregister, READ, true);
	tleregister = tleregister & ~0b000000000001111;
	tleregister = tleregister |  0b000000000001011;
	enc_tle5012_transfer(cfg, 0x0D, &tleregister, WRITE, true);

	// Interface Mode4
	enc_tle5012_transfer(cfg, 0x0E, &tleregister, READ, true);
	tleregister = tleregister & ~0b000000011111111;
	tleregister = tleregister |  0b000000000010000;
	enc_tle5012_transfer(cfg, 0x0E, &tleregister, WRITE, true);
	return false;
}

void enc_tle5012_deinit(TLE5012_config_t *cfg) {
	bool hwspi = false;
	if (hwspi){
		// if (cfg->spi_dev == NULL) {
		// 	return;
		// }

	    // palSetPadMode(cfg->miso_gpio, cfg->miso_pin, PAL_MODE_INPUT_PULLUP); check for spi vs ssc mode
		// palSetPadMode(cfg->sck_gpio, cfg->sck_pin, PAL_MODE_INPUT_PULLUP);
		// palSetPadMode(cfg->nss_gpio, cfg->nss_pin, PAL_MODE_INPUT_PULLUP);
		// palSetPadMode(cfg->mosi_gpio, cfg->mosi_pin, PAL_MODE_INPUT_PULLUP);

		// spiStop(cfg->spi_dev);

	} else {
		// sw spi	
		spi_bb_deinit(&(cfg->sw_spi)); 

		cfg->state.last_enc_angle = 0.0;
		cfg->state.spi_error_rate = 0.0;
	}
}

void enc_tle5012_routine(TLE5012_config_t *cfg) {
	float timestep = timer_seconds_elapsed_since(cfg->state.last_update_time);
	if (timestep > 1.0) {
		timestep = 1.0;
	}
	cfg->state.last_update_time = timer_time_now();

	uint16_t rx_data;
	uint8_t tle_status = enc_tle5012_transfer(cfg, REG_AVAL, &rx_data, READ, true);  // define register names values?

	if (tle_status == 0){
		uint16_t pos = rx_data & 0x7FFF;
		cfg->state.last_enc_angle = (float) pos * (360.0 / 32768.0); // 2^15 = 32768.0
		UTILS_LP_FAST(cfg->state.spi_error_rate, 0.0, timestep);
	}else{
		if (tle_status != 1 ) { // if not just a crc error

			// read/clear error reg
			uint16_t status_reg_dat;
			uint8_t tle_status_err = enc_tle5012_transfer(cfg, 0x00, &status_reg_dat, READ, true);
			
			// raise encoder exception? if over certain error rate. 10% maybe?
		}

		cfg->state.last_status_error = tle_status;
		++cfg->state.spi_error_cnt;
		UTILS_LP_FAST(cfg->state.spi_error_rate, 1.0, timestep);
	}
}

// get tle5012 temp in celsius.
uint8_t enc_tle5012_get_temperature(TLE5012_config_t *cfg, double *temperature) {
	uint16_t rawTemp = 0;
	uint8_t tle_status_err = enc_tle5012_transfer(cfg, REG_FSYNC, &rawTemp, READ, true);

	// extract 9 temp bits
	rawTemp = (rawTemp & (0x01FF)); 

	//check if the value received is positive or negative
	if (rawTemp & 0x0100)
	{
		rawTemp = rawTemp - 0x0200; // convert to 9 bit signed
	}
	// temperature = (rawTemp + TEMP_OFFSET) / (TEMP_DIV);
	*temperature = ((((int16_t) rawTemp) + 152.0) / (2.776));

	return (tle_status_err);
}

uint8_t enc_tle5012_transfer(TLE5012_config_t *cfg, uint8_t address, uint16_t *data, spi_direction read, bool safety) {
	const uint8_t upd = 0b0;
	uint16_t safety_word;

	// todo, make return value of this an enum with named errors?

	// command word:
	// [15] = rw, 1=read <- first bit transmitted
	// [14..11] = lock, 0000 (don't need this if not writing)
	// [10] = Update-Register Access, 0: Access to current values, 1: values in buffer
	// [9..4] = address, status=0x00, angle=0x02, speed=0x03
	// [3..0] = 4-bit Number of Data Words (if bits set to 0000B, no safety word is provided)


	uint16_t safeword; // this can probably be simplified. just pass safety?
	if (safety) {
		safeword = 0b001 << 0; // SAFE_0, just safety word
	} else {
		safeword = 0b000 << 0; // SAFE_0, no safety word
	}

	uint16_t command_word = (read << 15) | (upd << 10) | (address << 4)| (safeword << 0);
	spi_bb_begin(&(cfg->sw_spi));
	ssc_bb_transfer_16(&(cfg->sw_spi), &safety_word, &command_word, 1, 1); // send command
	ssc_bb_transfer_16(&(cfg->sw_spi), data, data, 1, !read); // read register
	if (safety) {
		ssc_bb_transfer_16(&(cfg->sw_spi), &safety_word, 0, 1, false); // read safety word
	}
	spi_bb_end(&(cfg->sw_spi));

	uint8_t status = checkSafety(command_word, safety_word, data, 1);
	// if (status != 0){
	// 	palClearPad(GPIOD, 1);
	// }else{
	// 	palSetPadMode(GPIOD, 1, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_HIGHEST);
	// 	palSetPad(GPIOD, 1);
	// }

	return status;
}


/*!
 * Function for calculation the CRC.
 * @param data byte long data for CRC check
 * @param length length of data
 * @return returns 8bit CRC
 */
uint8_t crc8(uint8_t *data, uint8_t length)
{
	uint32_t crc;
	int16_t i, bit;

	crc = TLE5012_CRC_SEED;
	for (i = 0; i < length; i++)
	{
		crc ^= data[i];
		for (bit = 0; bit < 8; bit++)
		{
			if ((crc & 0x80) != 0)
			{
				crc <<= 1;
				crc ^= TLE5012_CRC_POLYNOMIAL;
			}else{
				crc <<= 1;
			}
		}
	}
	return ((~crc) & TLE5012_CRC_SEED);
}


uint8_t checkSafety(uint16_t command, uint16_t safetyword, const uint16_t *readreg, uint16_t length){	
	/*
	safety word:
	[15]:Indication of chip reset or watchdog overflow (resets after readout) via SSC
	[14]: System error
	[13]: Interface access error
	[12]: Invalid angle value (produce vesc fault if 1)
	[11..8]: Sensor number response indicator
	[7..0]: crc 
	*/

	if (!((safetyword) & TLE5012_SYSTEM_ERROR_MASK)) {
		//SYSTEM_ERROR;
		// resetSafety();
		return 2;
	} else if (!((safetyword) & TLE5012_INTERFACE_ERROR_MASK)) {
		//INTERFACE_ACCESS_ERROR;
		//resetSafety();
		return 3;
	} else if (!((safetyword) & TLE5012_INV_ANGLE_ERROR_MASK)) {
		// INVALID_ANGLE_ERROR;
		//resetSafety();
		return 4;
	} else {
		//resetSafety();
		uint16_t lengthOfTemp = length * 2 + 2;
		uint8_t temp[lengthOfTemp];

		temp[0] = (uint8_t) (command >> 8);
		temp[1] = (uint8_t) (command);

		for (uint16_t i = 0; i < length; i++) {
			temp[2 + 2 * i] = (uint8_t) (readreg[i] >> 8);
			temp[2 + 2 * i + 1] = (uint8_t) (readreg[i]);
		}

		volatile uint8_t crcReceivedFinal = (uint8_t) safetyword;
		volatile uint8_t crc = crc8(temp, lengthOfTemp);

		if (crc == crcReceivedFinal) {
			// NO_ERROR;
			return 0;
		} else {
			// CRC_ERROR;
			return 1;
		}
	}
	return false;
}

// const Reg::BitField_t Reg::bitFields[] =
// {
// 	{REG_ACCESS_RU,  REG_STAT,    0x2,    1,  0x00,  0},       //!< 00 bits 0:0 SRST status watch dog
// 	{REG_ACCESS_R,   REG_STAT,    0x2,    1,  0x00,  0},       //!< 01 bits 1:1 SWD status watch dog
// 	{REG_ACCESS_R,   REG_STAT,    0x4,    2,  0x00,  0},       //!< 02 bits 2:2 SVR status voltage regulator
// 	{REG_ACCESS_R,   REG_STAT,    0x8,    3,  0x00,  0},       //!< 03 bits 3:3 SFUSE status fuses
// 	{REG_ACCESS_R,   REG_STAT,    0x10,   4,  0x00,  0},       //!< 04 bits 4:4 SDSPU status digital signal processing unit
// 	{REG_ACCESS_RU,  REG_STAT,    0x20,   5,  0x00,  0},       //!< 05 bits 5:5 SOV status overflow
// 	{REG_ACCESS_RU,  REG_STAT,    0x40,   6,  0x00,  0},       //!< 06 bits 6:6 SXYOL status X/Y data out limit
// 	{REG_ACCESS_RU,  REG_STAT,    0x80,   7,  0x00,  0},       //!< 07 bits 7:7 SMAGOL status magnitude out limit
// 	{REG_ACCESS_RES, REG_STAT,    0x100,  8,  0x00,  0},       //!< 08 bits 8:8 reserved
// 	{REG_ACCESS_R,   REG_STAT,    0x200,  9,  0x00,  0},       //!< 09 bits 9:9 SADCT status ADC test
// 	{REG_ACCESS_R,   REG_STAT,    0x400,  10, 0x00,  0},       //!< 10 bits 10:10 SROM status ROM
// 	{REG_ACCESS_RU,  REG_STAT,    0x800,  11, 0x00,  0},       //!< 11 bits 11:11 NOGMRXY no valid GMR XY Values
// 	{REG_ACCESS_RU,  REG_STAT,    0x1000, 12, 0x00,  0},       //!< 12 bits 12:12 NOGMRA no valid GMR Angle Value
// 	{REG_ACCESS_RW,  REG_STAT,    0x6000, 13, 0x00,  0},       //!< 13 bits 14:13 SNR slave number
// 	{REG_ACCESS_RU,  REG_STAT,    0x8000, 15, 0x00,  0},       //!< 14 bits 15:15 RDST read status

// 	{REG_ACCESS_RW,  REG_ACSTAT,  0x1,    0,  0x00,  1},       //!< 15 bits 0:0 ASRST Activation of Hardware Reset
// 	{REG_ACCESS_RWU, REG_ACSTAT,  0x2,    1,  0x00,  1},       //!< 16 bits 1:1 ASWD Enable DSPU Watch dog
// 	{REG_ACCESS_RWU, REG_ACSTAT,  0x4,    2,  0x00,  1},       //!< 17 bits 2:2 ASVR Enable Voltage regulator Check
// 	{REG_ACCESS_RWU, REG_ACSTAT,  0x8,    3,  0x00,  1},       //!< 18 bits 3:3 ASFUSE Activation Fuse CRC
// 	{REG_ACCESS_RWU, REG_ACSTAT,  0x10,   4,  0x00,  1},       //!< 19 bits 4:4 ASDSPU Activation DSPU BIST
// 	{REG_ACCESS_RWU, REG_ACSTAT,  0x20,   5,  0x00,  1},       //!< 20 bits 5:5 ASOV Enable of DSPU Overflow Check
// 	{REG_ACCESS_RWU, REG_ACSTAT,  0x40,   6,  0x00,  1},       //!< 21 bits 6:6 ASVECXY Activation of X,Y Out of Limit-Check
// 	{REG_ACCESS_RWU, REG_ACSTAT,  0x80,   7,  0x00,  1},       //!< 22 bits 7:7 ASVEGMAG Activation of Magnitude Check
// 	{REG_ACCESS_RES, REG_ACSTAT,  0x100,  8,  0x00,  1},       //!< 23 bits 8:8 Reserved
// 	{REG_ACCESS_RWU, REG_ACSTAT,  0x200,  9,  0x00,  1},       //!< 24 bits 9:9 ASADCT Enable ADC Test vector Check
// 	{REG_ACCESS_RWU, REG_ACSTAT,  0x400,  10, 0x00,  1},       //!< 25 bits 10:10 ASFRST Activation of Firmware Reset
// 	{REG_ACCESS_RES, REG_ACSTAT,  0xF800, 11, 0x00,  1},       //!< 26 bits 15:11 Reserved

// 	{REG_ACCESS_RU,  REG_AVAL,    0x7FFF, 0,  0x00,  2},       //!< 27 bits 14:0 ANGVAL Calculated Angle Value (signed 15-bit)
// 	{REG_ACCESS_R,   REG_AVAL,    0x8000, 15, 0x00,  2},       //!< 28 bits 15:15 RDAV Read Status, Angle Value

// 	{REG_ACCESS_RU,  REG_ASPD,    0x7FFF, 0,  0x00,  3},       //!< 29 bits 14:0 ANGSPD Signed value, where the sign bit [14] indicates the direction of the rotation
// 	{REG_ACCESS_R,   REG_ASPD,    0x8000, 15, 0x00,  3},       //!< 30 bits 15:15 RDAS Read Status, Angle Speed

// 	{REG_ACCESS_RU,  REG_AREV,    0xFF,   0,  0x00,  4},       //!< 31 bits 8:0 REVOL Revolution counter. Increments for every full rotation in counter-clockwise direction
// 	{REG_ACCESS_RWU, REG_AREV,    0x7E00, 9,  0x00,  4},       //!< 32 bits 14:9 FCNT Internal frame counter. Increments every update period
// 	{REG_ACCESS_R,   REG_AREV,    0x8000, 15, 0x00,  4},       //!< 33 its 15:15 RDREV Read Status, Revolution

// 	{REG_ACCESS_RWU, REG_FSYNC,   0xFF,   0,  0x00,  5},       //!< 34 bits 8:0 TEMPR Signed offset compensated temperature value
// 	{REG_ACCESS_RU,  REG_FSYNC,   0xFE00, 9,  0x00,  5},       //!< 35 bits 15:9 FSYNC Frame Synchronization Counter Value

// 	{REG_ACCESS_RW,  REG_MOD_1,   0x3,    0,  0x00,  6},       //!< 36 bits 1:0 IIFMOD Incremental Interface Mode
// 	{REG_ACCESS_RW,  REG_MOD_1,   0x4,    2,  0x00,  6},       //!< 37 bits 2:2 DSPUHOLD if DSPU is on hold, no watch dog reset is performed by DSPU
// 	{REG_ACCESS_RES, REG_MOD_1,   0x8,    3,  0x00,  6},       //!< 38 bits 3:3 Reserved1
// 	{REG_ACCESS_RW,  REG_MOD_1,   0x10,   4,  0x00,  6},       //!< 39 bits 4:4 CLKSEL switch to external clock at start-up only
// 	{REG_ACCESS_RES, REG_MOD_1,   0x3FE0, 5,  0x00,  6},       //!< 40 bits 13:5 Reserved2
// 	{REG_ACCESS_RW,  REG_MOD_1,   0x6000, 13, 0x00,  6},       //!< 41 bits 15:14 FIRMD Update Rate Setting

// 	{REG_ACCESS_RW,  REG_SIL,     0x7,    0,  0x00,  7},       //!< 42 bits 2:0 ADCTVX Test vector X
// 	{REG_ACCESS_RW,  REG_SIL,     0x38,   3,  0x00,  7},       //!< 43 bits 5:3 ADCTVY Test vector Y
// 	{REG_ACCESS_RW,  REG_SIL,     0x40,   6,  0x00,  7},       //!< 44 bits 6:6 ADCTVEN Sensor elements are internally disconnected and test voltages are connected to ADCs
// 	{REG_ACCESS_RES, REG_SIL,     0x380,  7,  0x00,  7},       //!< 45 bits 9:7 Reserved1
// 	{REG_ACCESS_RW,  REG_SIL,     0x400,  10, 0x00,  7},       //!< 46 bits 10:10 FUSEREL Triggers reload of default values from laser fuses into configuration registers
// 	{REG_ACCESS_RES, REG_SIL,     0x3800, 11, 0x00,  7},       //!< 47 bits 13:11 Reserved2
// 	{REG_ACCESS_RW,  REG_SIL,     0x4000, 14, 0x00,  7},       //!< 48 bits 14:14 FILTINV the X- and Y-signals are inverted. The angle output is then shifted by 180°
// 	{REG_ACCESS_RW,  REG_SIL,     0x8000, 15, 0x00,  7},       //!< 49 bits 15:15 FILTPAR the raw X-signal is routed also to the raw Y-signal input of the filter so SIN and COS signal should be identical

// 	{REG_ACCESS_RW,  REG_MOD_2,   0x3,    0,  0x00,  8},       //!< 50 bits 1:0 AUTOCAL Automatic calibration of offset and amplitude synchronicity for applications with full-turn
// 	{REG_ACCESS_RW,  REG_MOD_2,   0x4,    2,  0x00,  8},       //!< 51 bits 2:2 PREDICT Prediction of angle value based on current angle speed
// 	{REG_ACCESS_RW,  REG_MOD_2,   0x8,    3,  0x00,  8},       //!< 52 bits 3:3 ANGDIR Inverts angle and angle speed values and revolution counter behavior
// 	{REG_ACCESS_RW,  REG_MOD_2,   0x7FF0, 4,  0x00,  8},       //!< 53 bits 14:4 ANGRANGE Changes the representation of the angle output by multiplying the output with a factor ANG_RANGE/128
// 	{REG_ACCESS_RES, REG_MOD_2,   0x8000, 15, 0x00,  8},       //!< 54 bits 15:15 Reserved1

// 	{REG_ACCESS_RW,  REG_MOD_3,   0x3,    0,  0x00,  9},       //!< 55 bits 1:0 PADDRV Configuration of Pad-Driver
// 	{REG_ACCESS_RW,  REG_MOD_3,   0x4,    2,  0x00,  9},       //!< 56 bits 2:2 SSCOD SSC-Interface Data Pin Output Mode
// 	{REG_ACCESS_RW,  REG_MOD_3,   0x8,    3,  0x00,  9},       //!< 57 bits 3:3 SPIKEF Filters voltage spikes on input pads (IFC, SCK and CSQ)
// 	{REG_ACCESS_RW,  REG_MOD_3,   0xFFF0, 4,  0x00,  9},       //!< 58 bits 15:4 ANG_BASE Sets the 0° angle position (12 bit value). Angle base is factory-calibrated to make the 0° direction parallel to the edge of the chip

// 	{REG_ACCESS_RES, REG_OFFX,    0xF,    0,  0x00, 10},       //!< 59 bits 3:0 Reserved1
// 	{REG_ACCESS_RW,  REG_OFFX,    0xFFF0, 4,  0x00, 10},       //!< 60 bits 15:4 XOFFSET 12-bit signed integer value of raw X-signal offset correction at 25°C

// 	{REG_ACCESS_RES, REG_OFFY,    0xF,    0,  0x00, 11},       //!< 61 bits 3:0 Reserved1
// 	{REG_ACCESS_RW,  REG_OFFY,    0xFFF0, 4,  0x00, 11},       //!< 62 bits 15:4 YOFFSET 12-bit signed integer value of raw Y-signal offset correction at 25°C

// 	{REG_ACCESS_RES, REG_SYNCH,   0xF,    0,  0x00, 12},       //!< 63 bits 3:0 Reserved1
// 	{REG_ACCESS_RW,  REG_SYNCH,   0xFFF0, 4,  0x00, 12},       //!< 64 bits 15:4 SYNCH 12-bit signed integer value of amplitude synchronicity

// 	{REG_ACCESS_RW,  REG_IFAB,    0x3,    0,  0x00, 13},       //!< 65 bits 1:0 IFADHYST Hysteresis (multi-purpose)
// 	{REG_ACCESS_RW,  REG_IFAB,    0x4,    2,  0x00, 13},       //!< 66 bits 2:2 IFABOD IFA,IFB,IFC Output Mode
// 	{REG_ACCESS_RW,  REG_IFAB,    0x8,    3,  0x00, 13},       //!< 67 bits 3:3 FIRUDR Initial filter update rate (FIR)
// 	{REG_ACCESS_RW,  REG_IFAB,    0xFFF0, 4,  0x00, 13},       //!< 68 bits 15:4 ORTHO Orthogonality Correction of X and Y Components

// 	{REG_ACCESS_RW,  REG_MOD_4,   0x3,    0,  0x00, 14},       //!< 69 bits 1:0 IFMD Interface Mode on IFA,IFB,IFC
// 	{REG_ACCESS_RES, REG_MOD_4,   0x4,    2,  0x00, 14},       //!< 70 bits 2:2 Reserved1
// 	{REG_ACCESS_RW,  REG_MOD_4,   0x18,   3,  0x00, 14},       //!< 71 bits 4:3 IFABRES IIF resolution (multi-purpose)
// 	{REG_ACCESS_RW,  REG_MOD_4,   0x1E0,  5,  0x00, 14},       //!< 72 bits 8:5 HSMPLP Hall Switch mode (multi-purpose)
// 	{REG_ACCESS_RW,  REG_MOD_4,   0x7E00, 9,  0x00, 14},       //!< 73 bits 15:9 TCOXT 7-bit signed integer value of X-offset temperature coefficient

// 	{REG_ACCESS_RW,  REG_TCO_Y,   0x7F,   0,  0x00, 15},       //!< 74 bits 7:0 CRCPAR CRC of Parameters
// 	{REG_ACCESS_RW,  REG_TCO_Y,   0x80,   8,  0x00, 15},       //!< 75 bits 8:8 SBIST Startup-BIST
// 	{REG_ACCESS_RW,  REG_TCO_Y,   0x7E00, 9,  0x00, 15},       //!< 76 bits 15:9 TCOYT 7-bit signed integer value of Y-offset temperature coefficient

// 	{REG_ACCESS_R,   REG_ADC_X,   0xFFFF, 0,  0x00, 16},       //!< 77 bits 15:0 ADCX ADC value of X-GMR

// 	{REG_ACCESS_R,   REG_ADC_Y,   0xFFFF, 0,  0x00, 17},       //!< 78 bits 15:0 ADCY ADC value of Y-GMR

// 	{REG_ACCESS_RU,  REG_D_MAG,   0x3FF,  0,  0x00, 18},       //!< 79 bits 9:0 MAG Unsigned Angle Vector Magnitude after X, Y error compensation (due to temperature)
// 	{REG_ACCESS_RES, REG_D_MAG,   0xFC00, 10, 0x00, 18},       //!< 80 bits 15:10 Reserved1

// 	{REG_ACCESS_RU,  REG_T_RAW,   0x3FF,  0,  0x00, 19},       //!< 81 bits 9:0 TRAW Temperature Sensor Raw-Value at ADC without offset
// 	{REG_ACCESS_RES, REG_T_RAW,   0xFC00, 10, 0x00, 19},       //!< 82 bits 14:10 Reserved1
// 	{REG_ACCESS_RU,  REG_T_RAW,   0x8000, 15, 0x00, 19},       //!< 83 bits 15:15 TTGL Temperature Sensor Raw-Value Toggle toggles after every new temperature value

// 	{REG_ACCESS_RU,  REG_IIF_CNT, 0x7FFF, 0,  0x00, 20},       //!< 84 bits 14:0 IIFCNT 14 bit counter value of IIF increments
// 	{REG_ACCESS_RES, REG_IIF_CNT, 0x8000, 15, 0x00, 20},       //!< 85 bits 15:14 Reserved1

// 	{REG_ACCESS_R,   REG_T25O,    0x1FFF, 0,  0x00, 21},       //!< 86 bit 8:0 T250 Signed offset value at 25°C temperature; 1dig=0.36°C
// 	{REG_ACCESS_RES, REG_T25O,    0xFE00, 9,  0x00, 21},       //!< 87 bits 15:9 Reserved1
// };
