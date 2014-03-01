/*
 *  Copyright (C) 2009 Samsung Electronics
 *  Minkyu Kang <mk7.kang@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __MAX17040_BATTERY_H_
#define __MAX17040_BATTERY_H_

struct max17040_ini_data{
	const char *title;		/* Project title */
	int emptyadjustment;
	int fulladjustment;
	int rcomp0;				/* Starting RCOMP value */
	int tempcoup;			/* Temperature (hot) coeffiecient for RCOMP, should div 10 */
	int tempcodown;			/* Temperature (cold) coeffiecient for RCOMP, should div 10 */
	int ocvtest;			/* OCVTest vaule */
	int socchecka;			/* SOCCheck low value */
	int soccheckb;			/* SOCCheck high value */
	int bits;				/* 18 or 19 bit model */
	char data[128];			/* Model data. Ignore first/last 32 bytes, Used for EV kit only */
};

struct max17040_platform_data {
	int (*battery_online)(void);
	int (*charger_online)(void);
	int (*charger_enable)(void);
	struct max17040_ini_data ini_data;
};

#endif
