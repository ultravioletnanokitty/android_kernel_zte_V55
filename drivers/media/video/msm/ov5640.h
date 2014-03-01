/******************************************************************************
  @file    ov5640.h
  @brief   The camera ov5640 driver header file
  @author  chengjia 2011/02/22

  DESCRIPTION
  define data struction for using.
  ---------------------------------------------------------------------------
  Copyright (c) 2011 ZTE Incorporated. All Rights Reserved. 
  ZTE Proprietary and Confidential.
  ---------------------------------------------------------------------------
******************************************************************************/
#ifndef ov5640_H
#define ov5640_H
#include <linux/types.h>
#include <mach/board.h>

struct reg_addr_val_pair_struct {
	uint16_t	reg_addr;
	uint8_t	reg_val;
};

enum ov5640_test_mode_t {
	TEST_OFF,
	TEST_1,
	TEST_2,
	TEST_3
};

enum ov5640_resolution_t {
	QTR_SIZE,
	FULL_SIZE,
	INVALID_SIZE
};

enum ov5640_setting {
	RES_PREVIEW,
	RES_CAPTURE
};
enum ov5640_reg_update {
	/* Sensor egisters that need to be updated during initialization */
	REG_INIT,
	/* Sensor egisters that needs periodic I2C writes */
	UPDATE_PERIODIC,
	/* All the sensor Registers will be updated */
	UPDATE_ALL,
	/* Not valid update */
	UPDATE_INVALID
};
#endif

