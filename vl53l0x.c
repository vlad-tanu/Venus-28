/*
 *  TU/e 5EID0::LIBPYNQ Driver for VL53L0X TOF Sensor
 *
 *  Original C library: Larry Bank
 *  Adapted for PYNQ: Walthzer
 * 
 */

//
// VL53L0X time of flight range sensor
// Library to read the distance
// from the I2C bus
//
// by Larry Bank
//
// This code is based on Pololu's Arduino library
// https://github.com/pololu/vl53l0x-arduino
// (see LICENSE.txt for more info)
//
// My version is an attempt to simplify that code and 
// create a generic C library for Linux
//

#include <libpynq.h>
#include <vl53l0x.h>

#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static uint8_t readReg(vl53x *ptr_s, uint8_t ucAddr);
static unsigned short readReg16(vl53x *ptr_s, uint8_t ucAddr);
static void writeReg16(vl53x *ptr_s, uint8_t ucAddr, unsigned short usValue);
static void writeReg(vl53x *ptr_s, uint8_t ucAddr, uint8_t ucValue);
static void writeRegList(vl53x *ptr_s, uint8_t *ucList);
static int initSensor(vl53x *ptr_s, int);
static int performSingleRefCalibration(vl53x *ptr_s, uint8_t vhv_init_byte);
static int setMeasurementTimingBudget(vl53x *ptr_s, uint32_t budget_us);

#define calcMacroPeriod(vcsel_period_pclks) ((((uint32_t)2304 * (vcsel_period_pclks) * 1655) + 500) / 1000)
// Encode VCSEL pulse period register value from period in PCLKs
// based on VL53L0X_encode_vcsel_period()
#define encodeVcselPeriod(period_pclks) (((period_pclks) >> 1) - 1)

#define VL53L0X_SEQUENCE_ENABLE_FINAL_RANGE 0x80
#define VL53L0X_SEQUENCE_ENABLE_PRE_RANGE   0x40
#define VL53L0X_SEQUENCE_ENABLE_TCC         0x10
#define VL53L0X_SEQUENCE_ENABLE_DSS         0x08
#define VL53L0X_SEQUENCE_ENABLE_MSRC        0x04

typedef enum vcselperiodtype { VcselPeriodPreRange, VcselPeriodFinalRange } vcselPeriodType;
static int setVcselPulsePeriod(vl53x *ptr_s, vcselPeriodType type, uint8_t period_pclks);

typedef struct tagSequenceStepTimeouts
{
  uint16_t pre_range_vcsel_period_pclks, final_range_vcsel_period_pclks;

  uint16_t msrc_dss_tcc_mclks, pre_range_mclks, final_range_mclks;
  uint32_t msrc_dss_tcc_us,    pre_range_us,    final_range_us;
} SequenceStepTimeouts;

// VL53L0X internal registers
#define VL53L0X_REG_IDENTIFICATION_MODEL_ID		0xc0
#define VL53L0X_EXPECTED_MODEL_ID 0xEE
#define VL53L0X_REG_IDENTIFICATION_REVISION_ID		0xc2
#define VL53L0X_REG_SYSRANGE_START			0x00

#define VL53L0X_REG_RESULT_INTERRUPT_STATUS 		0x13
#define VL53L0X_RESULT_RANGE_STATUS      		0x14
#define VL53L0X_ALGO_PHASECAL_LIM                       0x30
#define VL53L0X_ALGO_PHASECAL_CONFIG_TIMEOUT            0x30

#define VL53L0X_GLOBAL_CONFIG_VCSEL_WIDTH               0x32
#define VL53L0X_FINAL_RANGE_CONFIG_VALID_PHASE_LOW      0x47
#define VL53L0X_FINAL_RANGE_CONFIG_VALID_PHASE_HIGH     0x48

#define VL53L0X_PRE_RANGE_CONFIG_VCSEL_PERIOD           0x50
#define VL53L0X_PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI      0x51
#define VL53L0X_PRE_RANGE_CONFIG_VALID_PHASE_LOW        0x56
#define VL53L0X_PRE_RANGE_CONFIG_VALID_PHASE_HIGH       0x57

#define VL53L0X_REG_MSRC_CONFIG_CONTROL                 0x60
#define VL53L0X_FINAL_RANGE_CONFIG_VCSEL_PERIOD         0x70
#define VL53L0X_FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI    0x71
#define VL53L0X_MSRC_CONFIG_TIMEOUT_MACROP              0x46
#define VL53L0X_FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT  0x44
#define VL53L0X_SYSRANGE_START                          0x00
#define VL53L0X_SYSTEM_SEQUENCE_CONFIG                  0x01
#define VL53L0X_SYSTEM_INTERRUPT_CONFIG_GPIO            0x0A
#define VL53L0X_RESULT_INTERRUPT_STATUS                 0x13
#define VL53L0X_VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV       0x89
#define VL53L0X_GLOBAL_CONFIG_SPAD_ENABLES_REF_0        0xB0
#define VL53L0X_GPIO_HV_MUX_ACTIVE_HIGH                 0x84
#define VL53L0X_SYSTEM_INTERRUPT_CLEAR                  0x0B

#define VL53L0X_REG_I2C_SLAVE_DEVICE_ADDRESS 0x8A

//
// Set IIC address of a VL53L0X Sensor
//
int tofSetAddress(iic_index_t iic, uint8_t addr, uint8_t newAddr)
{
  return iic_write_register(iic, addr, VL53L0X_REG_I2C_SLAVE_DEVICE_ADDRESS, &newAddr, 1);
}

//
// Connection test for a VL53L0X Sensor
// Model should be equal for all VL53L0X
//
int tofPing(iic_index_t iic, uint8_t addr)
{
  uint8_t model;
  iic_read_register(iic, addr, VL53L0X_REG_IDENTIFICATION_MODEL_ID, &model, 1);
  return (model != VL53L0X_EXPECTED_MODEL_ID);
}

//
// reads the calibration data and sets the device
// into auto sensing mode
//
int tofInit(vl53x *ptr_s, iic_index_t iic, uint8_t addr, int bLongRange)
{
  ptr_s->iic_index = iic;
  ptr_s->baseAddr = addr;
	return initSensor(ptr_s, bLongRange); // finally, initialize the magic numbers in the sensor

} /* tofInit() */



//
// Read a pair of registers as a 16-bit value
//
static unsigned short readReg16(vl53x *ptr_s, uint8_t ucAddr)
{
  uint8_t ucTemp[2];
  iic_read_register(ptr_s->iic_index, ptr_s->baseAddr, ucAddr, ucTemp, 2);

	return (unsigned short)((ucTemp[0]<<8) + ucTemp[1]);
} /* readReg16() */

//
// Read a single register value from I2C device
//
static uint8_t readReg(vl53x *ptr_s, uint8_t ucAddr)
{
  uint8_t ucTemp;

  iic_read_register(ptr_s->iic_index, ptr_s->baseAddr, ucAddr, &ucTemp, 1);

	return ucTemp;
} /* ReadReg() */

static void readMulti(vl53x *ptr_s, uint8_t ucAddr, uint8_t *pBuf, int iCount)
{

  iic_read_register(ptr_s->iic_index, ptr_s->baseAddr, ucAddr, pBuf, iCount);

} /* readMulti() */

static void writeMulti(vl53x *ptr_s, uint8_t ucAddr, uint8_t *pBuf, int iCount)
{

  iic_write_register(ptr_s->iic_index, ptr_s->baseAddr, ucAddr, pBuf, iCount);

} /* writeMulti() */
//
// Write a 16-bit value to a register
//
static void writeReg16(vl53x *ptr_s, uint8_t ucAddr, unsigned short usValue)
{
  uint8_t pBuf[2];

	pBuf[0] = (uint8_t)(usValue >> 8); // MSB first
	pBuf[1] = (uint8_t) usValue;

  iic_write_register(ptr_s->iic_index, ptr_s->baseAddr, ucAddr, pBuf, 2);
} /* writeReg16() */
//
// Write a single register/value pair
//
static void writeReg(vl53x *ptr_s, uint8_t ucAddr, uint8_t ucValue)
{

  iic_write_register(ptr_s->iic_index, ptr_s->baseAddr, ucAddr, &ucValue, 1);

} /* writeReg() */

//
// Write a list of register/value pairs to the I2C device
//
static void writeRegList(vl53x *ptr_s, uint8_t *ucList)
{
  uint8_t ucCount = *ucList++; // count is the first element in the list

	while (ucCount)
	{
    iic_write_register(ptr_s->iic_index, ptr_s->baseAddr, ucList[0], &(ucList[1]), 1);
		ucList += 2;
		ucCount--;
	}
} /* writeRegList() */

//
// Register init lists consist of the count followed by register/value pairs
//
uint8_t ucI2CMode[] = {4, 0x88,0x00, 0x80,0x01, 0xff,0x01, 0x00,0x00};
uint8_t ucI2CMode2[] = {3, 0x00,0x01, 0xff,0x00, 0x80,0x00};
uint8_t ucSPAD0[] = {4, 0x80,0x01, 0xff,0x01, 0x00,0x00, 0xff,0x06};
uint8_t ucSPAD1[] = {5, 0xff,0x07, 0x81,0x01, 0x80,0x01, 0x94,0x6b, 0x83,0x00};
uint8_t ucSPAD2[] = {4, 0xff,0x01, 0x00,0x01, 0xff,0x00, 0x80,0x00};
uint8_t ucSPAD[] = {5, 0xff,0x01, 0x4f,0x00, 0x4e,0x2c, 0xff,0x00, 0xb6,0xb4};
uint8_t ucDefTuning[] = {80, 0xff,0x01, 0x00,0x00, 0xff,0x00, 0x09,0x00,
0x10,0x00, 0x11,0x00, 0x24,0x01, 0x25,0xff, 0x75,0x00, 0xff,0x01, 0x4e,0x2c,
0x48,0x00, 0x30,0x20, 0xff,0x00, 0x30,0x09, 0x54,0x00, 0x31,0x04, 0x32,0x03,
0x40,0x83, 0x46,0x25, 0x60,0x00, 0x27,0x00, 0x50,0x06, 0x51,0x00, 0x52,0x96,
0x56,0x08, 0x57,0x30, 0x61,0x00, 0x62,0x00, 0x64,0x00, 0x65,0x00, 0x66,0xa0,
0xff,0x01, 0x22,0x32, 0x47,0x14, 0x49,0xff, 0x4a,0x00, 0xff,0x00, 0x7a,0x0a,
0x7b,0x00, 0x78,0x21, 0xff,0x01, 0x23,0x34, 0x42,0x00, 0x44,0xff, 0x45,0x26,
0x46,0x05, 0x40,0x40, 0x0e,0x06, 0x20,0x1a, 0x43,0x40, 0xff,0x00, 0x34,0x03,
0x35,0x44, 0xff,0x01, 0x31,0x04, 0x4b,0x09, 0x4c,0x05, 0x4d,0x04, 0xff,0x00,
0x44,0x00, 0x45,0x20, 0x47,0x08, 0x48,0x28, 0x67,0x00, 0x70,0x04, 0x71,0x01,
0x72,0xfe, 0x76,0x00, 0x77,0x00, 0xff,0x01, 0x0d,0x01, 0xff,0x00, 0x80,0x01,
0x01,0xf8, 0xff,0x01, 0x8e,0x01, 0x00,0x01, 0xff,0x00, 0x80,0x00};

int getSpadInfo(vl53x *ptr_s, uint8_t *pCount, uint8_t *pTypeIsAperture)
{
  int iTimeout;
  uint8_t ucTemp;
  #define VL53L0X_SPAD_MAX_TIMEOUT 50

  writeRegList(ptr_s, ucSPAD0);
  writeReg(ptr_s, 0x83, readReg(ptr_s, 0x83) | 0x04);
  writeRegList(ptr_s, ucSPAD1);
  iTimeout = 0;
  while(iTimeout < VL53L0X_SPAD_MAX_TIMEOUT)
  {
    if (readReg(ptr_s, 0x83) != 0x00) break;
    iTimeout++;
    sleep_msec(5);
  }
  if (iTimeout == VL53L0X_SPAD_MAX_TIMEOUT)
  {
    fprintf(stderr, "Timeout while waiting for SPAD info\n");
    return 0;
  }
  writeReg(ptr_s, 0x83,0x01);
  ucTemp = readReg(ptr_s, 0x92);
  *pCount = (ucTemp & 0x7f);
  *pTypeIsAperture = (ucTemp & 0x80);
  writeReg(ptr_s, 0x81,0x00);
  writeReg(ptr_s, 0xff,0x06);
  writeReg(ptr_s, 0x83, readReg(ptr_s, 0x83) & ~0x04);
  writeRegList(ptr_s, ucSPAD2);
  
  return 1;
} /* getSpadInfo() */

// Decode sequence step timeout in MCLKs from register value
// based on VL53L0X_decode_timeout()
// Note: the original function returned a uint32_t, but the return value is
// always stored in a uint16_t.
static uint16_t decodeTimeout(uint16_t reg_val)
{
  // format: "(LSByte * 2^MSByte) + 1"
  return (uint16_t)((reg_val & 0x00FF) <<
         (uint16_t)((reg_val & 0xFF00) >> 8)) + 1;
}

// Convert sequence step timeout from MCLKs to microseconds with given VCSEL period in PCLKs
// based on VL53L0X_calc_timeout_us()
static uint32_t timeoutMclksToMicroseconds(uint16_t timeout_period_mclks, uint8_t vcsel_period_pclks)
{
  uint32_t macro_period_ns = calcMacroPeriod(vcsel_period_pclks);

  return ((timeout_period_mclks * macro_period_ns) + (macro_period_ns / 2)) / 1000;
}

// Convert sequence step timeout from microseconds to MCLKs with given VCSEL period in PCLKs
// based on VL53L0X_calc_timeout_mclks()
static uint32_t timeoutMicrosecondsToMclks(uint32_t timeout_period_us, uint8_t vcsel_period_pclks)
{
  uint32_t macro_period_ns = calcMacroPeriod(vcsel_period_pclks);

  return (((timeout_period_us * 1000) + (macro_period_ns / 2)) / macro_period_ns);
}

// Encode sequence step timeout register value from timeout in MCLKs
// based on VL53L0X_encode_timeout()
// Note: the original function took a uint16_t, but the argument passed to it
// is always a uint16_t.
static uint16_t encodeTimeout(uint16_t timeout_mclks)
{
  // format: "(LSByte * 2^MSByte) + 1"

  uint32_t ls_byte = 0;
  uint16_t ms_byte = 0;

  if (timeout_mclks > 0)
  {
    ls_byte = timeout_mclks - 1;

    while ((ls_byte & 0xFFFFFF00) > 0)
    {
      ls_byte >>= 1;
      ms_byte++;
    }

    return (ms_byte << 8) | (ls_byte & 0xFF);
  }
  else { return 0; }
}

static void getSequenceStepTimeouts(vl53x *ptr_s, uint8_t enables, SequenceStepTimeouts * timeouts)
{
  timeouts->pre_range_vcsel_period_pclks = ((readReg(ptr_s, VL53L0X_PRE_RANGE_CONFIG_VCSEL_PERIOD) +1) << 1);

  timeouts->msrc_dss_tcc_mclks = readReg(ptr_s, VL53L0X_MSRC_CONFIG_TIMEOUT_MACROP) + 1;
  timeouts->msrc_dss_tcc_us =
    timeoutMclksToMicroseconds(timeouts->msrc_dss_tcc_mclks,
                               timeouts->pre_range_vcsel_period_pclks);

  timeouts->pre_range_mclks =
    decodeTimeout(readReg16(ptr_s, VL53L0X_PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI));
  timeouts->pre_range_us =
    timeoutMclksToMicroseconds(timeouts->pre_range_mclks,
                               timeouts->pre_range_vcsel_period_pclks);

  timeouts->final_range_vcsel_period_pclks = ((readReg(ptr_s, VL53L0X_FINAL_RANGE_CONFIG_VCSEL_PERIOD) +1) << 1);

  timeouts->final_range_mclks =
    decodeTimeout(readReg16(ptr_s, VL53L0X_FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI));

  if (enables & VL53L0X_SEQUENCE_ENABLE_PRE_RANGE)
  {
    timeouts->final_range_mclks -= timeouts->pre_range_mclks;
  }

  timeouts->final_range_us =
    timeoutMclksToMicroseconds(timeouts->final_range_mclks,
                               timeouts->final_range_vcsel_period_pclks);
} /* getSequenceStepTimeouts() */


// Set the VCSEL (vertical cavity surface emitting laser) pulse period for the
// given period type (pre-range or final range) to the given value in PCLKs.
// Longer periods seem to increase the potential range of the sensor.
// Valid values are (even numbers only):
//  pre:  12 to 18 (initialized default: 14)
//  final: 8 to 14 (initialized default: 10)
// based on VL53L0X_set_vcsel_pulse_period()
static int setVcselPulsePeriod(vl53x *ptr_s, vcselPeriodType type, uint8_t period_pclks)
{
  uint8_t vcsel_period_reg = encodeVcselPeriod(period_pclks);

  uint8_t enables;
  SequenceStepTimeouts timeouts;

  enables = readReg(ptr_s, VL53L0X_SYSTEM_SEQUENCE_CONFIG);
  getSequenceStepTimeouts(ptr_s, enables, &timeouts);

  // "Apply specific settings for the requested clock period"
  // "Re-calculate and apply timeouts, in macro periods"

  // "When the VCSEL period for the pre or final range is changed,
  // the corresponding timeout must be read from the device using
  // the current VCSEL period, then the new VCSEL period can be
  // applied. The timeout then must be written back to the device
  // using the new VCSEL period.
  //
  // For the MSRC timeout, the same applies - this timeout being
  // dependant on the pre-range vcsel period."


  if (type == VcselPeriodPreRange)
  {
    // "Set phase check limits"
    switch (period_pclks)
    {
      case 12:
        writeReg(ptr_s, VL53L0X_PRE_RANGE_CONFIG_VALID_PHASE_HIGH, 0x18);
        break;

      case 14:
        writeReg(ptr_s, VL53L0X_PRE_RANGE_CONFIG_VALID_PHASE_HIGH, 0x30);
        break;

      case 16:
        writeReg(ptr_s, VL53L0X_PRE_RANGE_CONFIG_VALID_PHASE_HIGH, 0x40);
        break;

      case 18:
        writeReg(ptr_s, VL53L0X_PRE_RANGE_CONFIG_VALID_PHASE_HIGH, 0x50);
        break;

      default:
        // invalid period
        return 0;
    }
    writeReg(ptr_s, VL53L0X_PRE_RANGE_CONFIG_VALID_PHASE_LOW, 0x08);

    // apply new VCSEL period
    writeReg(ptr_s, VL53L0X_PRE_RANGE_CONFIG_VCSEL_PERIOD, vcsel_period_reg);

    // update timeouts

    // set_sequence_step_timeout() begin
    // (SequenceStepId == VL53L0X_SEQUENCESTEP_PRE_RANGE)

    uint16_t new_pre_range_timeout_mclks =
      timeoutMicrosecondsToMclks(timeouts.pre_range_us, period_pclks);

    writeReg16(ptr_s, VL53L0X_PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI,
      encodeTimeout(new_pre_range_timeout_mclks));

    // set_sequence_step_timeout() end

    // set_sequence_step_timeout() begin
    // (SequenceStepId == VL53L0X_SEQUENCESTEP_MSRC)

    uint16_t new_msrc_timeout_mclks =
      timeoutMicrosecondsToMclks(timeouts.msrc_dss_tcc_us, period_pclks);

    writeReg(ptr_s, VL53L0X_MSRC_CONFIG_TIMEOUT_MACROP,
      (new_msrc_timeout_mclks > 256) ? 255 : (new_msrc_timeout_mclks - 1));

    // set_sequence_step_timeout() end
  }
  else if (type == VcselPeriodFinalRange)
  {
    switch (period_pclks)
    {
      case 8:
        writeReg(ptr_s, VL53L0X_FINAL_RANGE_CONFIG_VALID_PHASE_HIGH, 0x10);
        writeReg(ptr_s, VL53L0X_FINAL_RANGE_CONFIG_VALID_PHASE_LOW,  0x08);
        writeReg(ptr_s, VL53L0X_GLOBAL_CONFIG_VCSEL_WIDTH, 0x02);
        writeReg(ptr_s, VL53L0X_ALGO_PHASECAL_CONFIG_TIMEOUT, 0x0C);
        writeReg(ptr_s, 0xFF, 0x01);
        writeReg(ptr_s, VL53L0X_ALGO_PHASECAL_LIM, 0x30);
        writeReg(ptr_s, 0xFF, 0x00);
        break;

      case 10:
        writeReg(ptr_s, VL53L0X_FINAL_RANGE_CONFIG_VALID_PHASE_HIGH, 0x28);
        writeReg(ptr_s, VL53L0X_FINAL_RANGE_CONFIG_VALID_PHASE_LOW,  0x08);
        writeReg(ptr_s, VL53L0X_GLOBAL_CONFIG_VCSEL_WIDTH, 0x03);
        writeReg(ptr_s, VL53L0X_ALGO_PHASECAL_CONFIG_TIMEOUT, 0x09);
        writeReg(ptr_s, 0xFF, 0x01);
        writeReg(ptr_s, VL53L0X_ALGO_PHASECAL_LIM, 0x20);
        writeReg(ptr_s, 0xFF, 0x00);
        break;

      case 12:
        writeReg(ptr_s, VL53L0X_FINAL_RANGE_CONFIG_VALID_PHASE_HIGH, 0x38);
        writeReg(ptr_s, VL53L0X_FINAL_RANGE_CONFIG_VALID_PHASE_LOW,  0x08);
        writeReg(ptr_s, VL53L0X_GLOBAL_CONFIG_VCSEL_WIDTH, 0x03);
        writeReg(ptr_s, VL53L0X_ALGO_PHASECAL_CONFIG_TIMEOUT, 0x08);
        writeReg(ptr_s, 0xFF, 0x01);
        writeReg(ptr_s, VL53L0X_ALGO_PHASECAL_LIM, 0x20);
        writeReg(ptr_s, 0xFF, 0x00);
        break;

      case 14:
        writeReg(ptr_s, VL53L0X_FINAL_RANGE_CONFIG_VALID_PHASE_HIGH, 0x48);
        writeReg(ptr_s, VL53L0X_FINAL_RANGE_CONFIG_VALID_PHASE_LOW,  0x08);
        writeReg(ptr_s, VL53L0X_GLOBAL_CONFIG_VCSEL_WIDTH, 0x03);
        writeReg(ptr_s, VL53L0X_ALGO_PHASECAL_CONFIG_TIMEOUT, 0x07);
        writeReg(ptr_s, 0xFF, 0x01);
        writeReg(ptr_s, VL53L0X_ALGO_PHASECAL_LIM, 0x20);
        writeReg(ptr_s, 0xFF, 0x00);
        break;

      default:
        // invalid period
        return 0;
    }

    // apply new VCSEL period
    writeReg(ptr_s, VL53L0X_FINAL_RANGE_CONFIG_VCSEL_PERIOD, vcsel_period_reg);

    // update timeouts

    // set_sequence_step_timeout() begin
    // (SequenceStepId == VL53L0X_SEQUENCESTEP_FINAL_RANGE)

    // "For the final range timeout, the pre-range timeout
    //  must be added. To do this both final and pre-range
    //  timeouts must be expressed in macro periods MClks
    //  because they have different vcsel periods."

    uint16_t new_final_range_timeout_mclks =
      timeoutMicrosecondsToMclks(timeouts.final_range_us, period_pclks);

    if (enables & VL53L0X_SEQUENCE_ENABLE_PRE_RANGE)
    {
      new_final_range_timeout_mclks += timeouts.pre_range_mclks;
    }

    writeReg16(ptr_s, VL53L0X_FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI,
    encodeTimeout(new_final_range_timeout_mclks));

    // set_sequence_step_timeout end
  }
  else
  {
    // invalid type
    return 0;
  }

  // "Finally, the timing budget must be re-applied"

  setMeasurementTimingBudget(ptr_s, ptr_s->measurement_timing_budget_us);

  // "Perform the phase calibration. This is needed after changing on vcsel period."
  // VL53L0X_perform_phase_calibration() begin

  uint8_t sequence_config = readReg(ptr_s, VL53L0X_SYSTEM_SEQUENCE_CONFIG);
  writeReg(ptr_s, VL53L0X_SYSTEM_SEQUENCE_CONFIG, 0x02);
  performSingleRefCalibration(ptr_s, 0x0);
  writeReg(ptr_s, VL53L0X_SYSTEM_SEQUENCE_CONFIG, sequence_config);

  // VL53L0X_perform_phase_calibration() end

  return 1;
}

// Set the measurement timing budget in microseconds, which is the time allowed
// for one measurement; the ST API and this library take care of splitting the
// timing budget among the sub-steps in the ranging sequence. A longer timing
// budget allows for more accurate measurements. Increasing the budget by a
// factor of N decreases the range measurement standard deviation by a factor of
// sqrt(N). Defaults to about 33 milliseconds; the minimum is 20 ms.
// based on VL53L0X_set_measurement_timing_budget_micro_seconds()
int setMeasurementTimingBudget(vl53x *ptr_s, uint32_t budget_us)
{
uint32_t used_budget_us;
uint32_t final_range_timeout_us;
uint16_t final_range_timeout_mclks;

  uint8_t enables;
  SequenceStepTimeouts timeouts;

  uint16_t const StartOverhead      = 1320; // note that this is different than the value in get_
  uint16_t const EndOverhead        = 960;
  uint16_t const MsrcOverhead       = 660;
  uint16_t const TccOverhead        = 590;
  uint16_t const DssOverhead        = 690;
  uint16_t const PreRangeOverhead   = 660;
  uint16_t const FinalRangeOverhead = 550;

  uint32_t const MinTimingBudget = 20000;

  if (budget_us < MinTimingBudget) { return 0; }

  used_budget_us = StartOverhead + EndOverhead;

  enables = readReg(ptr_s, VL53L0X_SYSTEM_SEQUENCE_CONFIG);
  getSequenceStepTimeouts(ptr_s, enables, &timeouts);

  if (enables & VL53L0X_SEQUENCE_ENABLE_TCC)
  {
    used_budget_us += (timeouts.msrc_dss_tcc_us + TccOverhead);
  }

  if (enables & VL53L0X_SEQUENCE_ENABLE_DSS)
  {
    used_budget_us += 2 * (timeouts.msrc_dss_tcc_us + DssOverhead);
  }
  else if (enables & VL53L0X_SEQUENCE_ENABLE_MSRC)
  {
    used_budget_us += (timeouts.msrc_dss_tcc_us + MsrcOverhead);
  }

  if (enables & VL53L0X_SEQUENCE_ENABLE_PRE_RANGE)
  {
    used_budget_us += (timeouts.pre_range_us + PreRangeOverhead);
  }

  if (enables & VL53L0X_SEQUENCE_ENABLE_FINAL_RANGE)
  {
    used_budget_us += FinalRangeOverhead;

    // "Note that the final range timeout is determined by the timing
    // budget and the sum of all other timeouts within the sequence.
    // If there is no room for the final range timeout, then an error
    // will be set. Otherwise the remaining time will be applied to
    // the final range."

    if (used_budget_us > budget_us)
    {
      // "Requested timeout too big."
      return 0;
    }

    final_range_timeout_us = budget_us - used_budget_us;

    // set_sequence_step_timeout() begin
    // (SequenceStepId == VL53L0X_SEQUENCESTEP_FINAL_RANGE)

    // "For the final range timeout, the pre-range timeout
    //  must be added. To do this both final and pre-range
    //  timeouts must be expressed in macro periods MClks
    //  because they have different vcsel periods."

    final_range_timeout_mclks =
      timeoutMicrosecondsToMclks(final_range_timeout_us,
                                 timeouts.final_range_vcsel_period_pclks);

    if (enables & VL53L0X_SEQUENCE_ENABLE_PRE_RANGE)
    {
      final_range_timeout_mclks += timeouts.pre_range_mclks;
    }

    writeReg16(ptr_s, VL53L0X_FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI,
      encodeTimeout(final_range_timeout_mclks));

    // set_sequence_step_timeout() end

    ptr_s->measurement_timing_budget_us = budget_us; // store for internal reuse
  }
  return 1;
}

uint32_t getMeasurementTimingBudget(vl53x *ptr_s)
{
  uint8_t enables;
  SequenceStepTimeouts timeouts;

  uint16_t const StartOverhead     = 1910; // note that this is different than the value in set_
  uint16_t const EndOverhead        = 960;
  uint16_t const MsrcOverhead       = 660;
  uint16_t const TccOverhead        = 590;
  uint16_t const DssOverhead        = 690;
  uint16_t const PreRangeOverhead   = 660;
  uint16_t const FinalRangeOverhead = 550;

  // "Start and end overhead times always present"
  uint32_t budget_us = StartOverhead + EndOverhead;

  enables = readReg(ptr_s, VL53L0X_SYSTEM_SEQUENCE_CONFIG);
  getSequenceStepTimeouts(ptr_s, enables, &timeouts);

  if (enables & VL53L0X_SEQUENCE_ENABLE_TCC)
  {
    budget_us += (timeouts.msrc_dss_tcc_us + TccOverhead);
  }

  if (enables & VL53L0X_SEQUENCE_ENABLE_DSS)
  {
    budget_us += 2 * (timeouts.msrc_dss_tcc_us + DssOverhead);
  }
  else if (enables & VL53L0X_SEQUENCE_ENABLE_MSRC)
  {
    budget_us += (timeouts.msrc_dss_tcc_us + MsrcOverhead);
  }

  if (enables & VL53L0X_SEQUENCE_ENABLE_PRE_RANGE)
  {
    budget_us += (timeouts.pre_range_us + PreRangeOverhead);
  }

  if (enables & VL53L0X_SEQUENCE_ENABLE_FINAL_RANGE)
  {
    budget_us += (timeouts.final_range_us + FinalRangeOverhead);
  }

  ptr_s->measurement_timing_budget_us = budget_us; // store for internal reuse
  return budget_us;
}

int performSingleRefCalibration(vl53x *ptr_s, uint8_t vhv_init_byte)
{
  writeReg(ptr_s, VL53L0X_SYSRANGE_START, 0x01 | vhv_init_byte); // VL53L0X_REG_SYSRANGE_MODE_START_STOP

  int iTimeout = 0;
  while ((readReg(ptr_s, VL53L0X_RESULT_INTERRUPT_STATUS) & 0x07) == 0)
  {
    iTimeout++;
    sleep_msec(5);
    if (iTimeout > 100) { return 0; }
  }

  writeReg(ptr_s, VL53L0X_SYSTEM_INTERRUPT_CLEAR, 0x01);

  writeReg(ptr_s, VL53L0X_SYSRANGE_START, 0x00);

  return 1;
} /* performSingleRefCalibration() */

//
// Initialize the vl53l0x
//
int initSensor(vl53x *ptr_s, int bLongRangeMode)
{
  uint8_t spad_count=0, spad_type_is_aperture=0, ref_spad_map[6];
  uint8_t ucFirstSPAD, ucSPADsEnabled;
  int i;

// set 2.8V mode
  writeReg(ptr_s, VL53L0X_VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV,
  readReg(ptr_s, VL53L0X_VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV) | 0x01); // set bit 0
  // Set I2C standard mode
  writeRegList(ptr_s, ucI2CMode);
  ptr_s->stop_variable = readReg(ptr_s, 0x91);
  writeRegList(ptr_s, ucI2CMode2);
  // disable SIGNAL_RATE_MSRC (bit 1) and SIGNAL_RATE_PRE_RANGE (bit 4) limit checks
  writeReg(ptr_s, VL53L0X_REG_MSRC_CONFIG_CONTROL, readReg(ptr_s, VL53L0X_REG_MSRC_CONFIG_CONTROL) | 0x12);
  // Q9.7 fixed point format (9 integer bits, 7 fractional bits)
  writeReg16(ptr_s, VL53L0X_FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT, 32); // 0.25
  writeReg(ptr_s, VL53L0X_SYSTEM_SEQUENCE_CONFIG, 0xFF);
  getSpadInfo(ptr_s, &spad_count, &spad_type_is_aperture);
  readMulti(ptr_s, VL53L0X_GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, 6);
  //printf("initial spad map: %02x,%02x,%02x,%02x,%02x,%02x\n", ref_spad_map[0], ref_spad_map[1], ref_spad_map[2], ref_spad_map[3], ref_spad_map[4], ref_spad_map[5]);
  writeRegList(ptr_s, ucSPAD);
  ucFirstSPAD = (spad_type_is_aperture) ? 12: 0;
  ucSPADsEnabled = 0;
// clear bits for unused SPADs
  for (i=0; i<48; i++)
  {
    if (i < ucFirstSPAD || ucSPADsEnabled == spad_count)
    {
      ref_spad_map[i>>3] &= ~(1<<(i & 7));
    }
    else if (ref_spad_map[i>>3] & (1<< (i & 7)))
    {
      ucSPADsEnabled++;
    }
  } // for i
  writeMulti(ptr_s, VL53L0X_GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, 6);
  //printf("final spad map: %02x,%02x,%02x,%02x,%02x,%02x\n", ref_spad_map[0], ref_spad_map[1], ref_spad_map[2], ref_spad_map[3], ref_spad_map[4], ref_spad_map[5]);

// load default tuning settings
  writeRegList(ptr_s, ucDefTuning); // long list of magic numbers

// change some settings for long range mode
  if (bLongRangeMode)
  {
	writeReg16(ptr_s, VL53L0X_FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT, 13); // 0.1
	setVcselPulsePeriod(ptr_s, VcselPeriodPreRange, 18);
	setVcselPulsePeriod(ptr_s, VcselPeriodFinalRange, 14);
  }

// set interrupt configuration to "new sample ready"
  writeReg(ptr_s, VL53L0X_SYSTEM_INTERRUPT_CONFIG_GPIO, 0x04);
  writeReg(ptr_s, VL53L0X_GPIO_HV_MUX_ACTIVE_HIGH, readReg(ptr_s, VL53L0X_GPIO_HV_MUX_ACTIVE_HIGH) & ~0x10); // active low
  writeReg(ptr_s, VL53L0X_SYSTEM_INTERRUPT_CLEAR, 0x01);
  ptr_s->measurement_timing_budget_us = getMeasurementTimingBudget(ptr_s);
  writeReg(ptr_s, VL53L0X_SYSTEM_SEQUENCE_CONFIG, 0xe8);
  setMeasurementTimingBudget(ptr_s, ptr_s->measurement_timing_budget_us);
  writeReg(ptr_s, VL53L0X_SYSTEM_SEQUENCE_CONFIG, 0x01);

  if (!performSingleRefCalibration(ptr_s, 0x40)) { return 1; }
  writeReg(ptr_s, VL53L0X_SYSTEM_SEQUENCE_CONFIG, 0x02);
  if (!performSingleRefCalibration(ptr_s, 0x00)) { return 1; }
  writeReg(ptr_s, VL53L0X_SYSTEM_SEQUENCE_CONFIG, 0xe8);
  return 0;
} /* initSensor() */

uint16_t readRangeContinuousMillimeters(vl53x *ptr_s)
{
int iTimeout = 0;
uint16_t range;

  while ((readReg(ptr_s, VL53L0X_RESULT_INTERRUPT_STATUS) & 0x07) == 0)
  {
    iTimeout++;
    sleep_msec(50);
    if (iTimeout > 50)
    {
      return -1;
    }
  }

  // assumptions: Linearity Corrective Gain is 1000 (default);
  // fractional ranging is not enabled
  range = readReg16(ptr_s, VL53L0X_RESULT_RANGE_STATUS + 10);

  writeReg(ptr_s, VL53L0X_SYSTEM_INTERRUPT_CLEAR, 0x01);

  return range;
}
//
// Read the current distance in mm
//
uint32_t tofReadDistance(vl53x *sensor)
{
int iTimeout;

  writeReg(sensor, 0x80, 0x01);
  writeReg(sensor, 0xFF, 0x01);
  writeReg(sensor, 0x00, 0x00);
  writeReg(sensor, 0x91, sensor->stop_variable);
  writeReg(sensor, 0x00, 0x01);
  writeReg(sensor, 0xFF, 0x00);
  writeReg(sensor, 0x80, 0x00);

  writeReg(sensor, VL53L0X_SYSRANGE_START, 0x01);

  // "Wait until start bit has been cleared"
  iTimeout = 0;
  while (readReg(sensor, VL53L0X_SYSRANGE_START) & 0x01)
  {
    iTimeout++;
    sleep_msec(50);
    if (iTimeout > 50)
    {
      return -1;
    }
  }

  return readRangeContinuousMillimeters(sensor);

} /* tofReadDistance() */

int tofGetModel(vl53x *sensor, uint8_t *model, uint8_t *revision)
{
uint8_t ucTemp[2];
int i;

	if (model)
	{
		i = iic_read_register(sensor->iic_index, sensor->baseAddr, VL53L0X_REG_IDENTIFICATION_MODEL_ID, ucTemp, 1);
		if (i == 0) //0 on succes aka no error
			*model = ucTemp[0];
    else
      return 1;
	}
	if (revision)
	{
      i = iic_read_register(sensor->iic_index, sensor->baseAddr, VL53L0X_REG_IDENTIFICATION_REVISION_ID, ucTemp, 1);
		if (i == 0)
			*revision = ucTemp[0];
    else
      return 1;
	}
	return 0;

} /* tofGetModel() */
