/*
 * Copyright (c) 2021 Innoseis B.V
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_SENSOR_TMP11X_H_
#define ZEPHYR_INCLUDE_DRIVERS_SENSOR_TMP11X_H_

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <sys/types.h>

enum sensor_attribute_tmp_11x {
	/** Turn on power saving/one shot mode */
	SENSOR_ATTR_TMP11X_ONE_SHOT_MODE = SENSOR_ATTR_PRIV_START,
	/** Shutdown the sensor */
	SENSOR_ATTR_TMP11X_SHUTDOWN_MODE,
	/** Turn on continuous conversion */
	SENSOR_ATTR_TMP11X_CONTINUOUS_CONVERSION_MODE,
	/** Configure alert pin polarity */
	SENSOR_ATTR_TMP11X_ALERT_PIN_POLARITY,
	/** Configure alert mode */
	SENSOR_ATTR_TMP11X_ALERT_MODE,
	/** Configure alert pin mode for alert or DR*/
	SENSOR_ATTR_TMP11X_ALERT_PIN_SELECT,
};

/* Alert pin support macros */
#define TMP11X_ALERT_PIN_ACTIVE_LOW  0
#define TMP11X_ALERT_PIN_ACTIVE_HIGH 1
#define TMP11X_ALERT_ALERT_MODE      0
#define TMP11X_ALERT_THERM_MODE      1
#define TMP11X_ALERT_PIN_ALERT_SEL   0
#define TMP11X_ALERT_PIN_DR_SEL      1

#define EEPROM_TMP11X_SIZE (4 * sizeof(uint16_t))

int tmp11x_eeprom_read(const struct device *dev, off_t offset, void *data,
		       size_t len);

int tmp11x_eeprom_write(const struct device *dev, off_t offset,
			const void *data, size_t len);

#endif /* ZEPHYR_INCLUDE_DRIVERS_SENSOR_TMP11X_H_ */
