/******************************************************************************
  @file    mt9d115.h
  @brief   The camera mt9d115 driver header file
  @author  liyibo 2011/03/22

  DESCRIPTION
  define data struction for using.
  ---------------------------------------------------------------------------
  Copyright (c) 2011 ZTE Incorporated. All Rights Reserved. 
  ZTE Proprietary and Confidential.
  ---------------------------------------------------------------------------
******************************************************************************/
#ifndef mt9d115_H
#define mt9d115_H
#include <linux/types.h>
#include <mach/board.h>

struct reg_addr_val_pair_struct {
	uint16_t	reg_addr;
	uint16_t	reg_val;
};

enum mt9d115_test_mode_t {
	TEST_OFF,
	TEST_1,
	TEST_2,
	TEST_3
};

enum mt9d115_resolution_t {
	QTR_SIZE,
	FULL_SIZE,
	INVALID_SIZE
};

enum mt9d115_setting {
	RES_PREVIEW,
	RES_CAPTURE
};
enum mt9d115_reg_update {
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

