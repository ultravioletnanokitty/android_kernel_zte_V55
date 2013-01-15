/*
 * LED Core
 *
 * Copyright 2005 Openedhand Ltd.
 *
 * Author: Richard Purdie <rpurdie@openedhand.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
 /*===========================================================================

                        EDIT HISTORY FOR V11

when              comment tag        who                  what, where, why                       
2011/06/15    liyuan0006            liyuan               add leds asynchronous blink function
===========================================================================*/
#ifndef __LEDS_H_INCLUDED
#define __LEDS_H_INCLUDED

#include <linux/device.h>
#include <linux/rwsem.h>
#include <linux/leds.h>
#include <linux/workqueue.h>

#ifdef CONFIG_HAS_EARLYSUSPEND

extern struct workqueue_struct *suspend_work_queue;
extern int queue_brightness_change(struct led_classdev *led_cdev,
	enum led_brightness value);

struct deferred_brightness_change {
	struct work_struct brightness_change_work;
	struct led_classdev *led_cdev;
	enum led_brightness value;
};

#endif

static inline void led_set_brightness(struct led_classdev *led_cdev,
					enum led_brightness value)
{
	if (value > led_cdev->max_brightness)
		value = led_cdev->max_brightness;
	led_cdev->brightness = value;
	if (!(led_cdev->flags & LED_SUSPENDED)) {
#ifdef CONFIG_HAS_EARLYSUSPEND
		if (queue_brightness_change(led_cdev, value) != 0)
#endif
			led_cdev->brightness_set(led_cdev, value);
	}
}

/** ZTE_MODIFY liyuan to add leds asynchronous blink function , 2011-06-15*/
#ifdef CONFIG_LEDS_ZTE
static inline void led_set_blink(struct led_classdev *led_cdev,
					enum led_blink value)
{	      
      led_cdev->blink=value;       
      
      led_cdev->blink_set(led_cdev, value);		
}

/** ZTE_MODIFY yuanbo to add leds  blink function , 2011-10-19*/
static inline void led_set_grpfreq(struct led_classdev *led_cdev,
					int  value)
{	      

      if( value != 0)
          led_cdev->grpfreq=value;       
	
}

static inline void led_set_grppwm(struct led_classdev *led_cdev,
					int  value)
{	      
      
      if( value  < 0)  value = 0;
      if( value > 255) value = 255;

      led_cdev->grppwm=value; 

}
/** ZTE_MODIFY yuanbo 2011-10-19 end */
#endif
/** ZTE_MODIFY end */

static inline int led_get_brightness(struct led_classdev *led_cdev)
{
	return led_cdev->brightness;
}

extern struct rw_semaphore leds_list_lock;
extern struct list_head leds_list;

#ifdef CONFIG_LEDS_TRIGGERS
void led_trigger_set_default(struct led_classdev *led_cdev);
void led_trigger_set(struct led_classdev *led_cdev,
			struct led_trigger *trigger);
void led_trigger_remove(struct led_classdev *led_cdev);
#else
#define led_trigger_set_default(x) do {} while (0)
#define led_trigger_set(x, y) do {} while (0)
#define led_trigger_remove(x) do {} while (0)
#endif

ssize_t led_trigger_store(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count);
ssize_t led_trigger_show(struct device *dev, struct device_attribute *attr,
			char *buf);

#endif	/* __LEDS_H_INCLUDED */
