/*
 * Copyright (C) 2014 Intel Corporation.
 */

#include <dirent.h>
#include <utils/Log.h>
#include <hardware/sensors.h>
#include "enumeration.h"
#include "description.h"
#include "utils.h"
#include "transform.h"

/*
 * This table maps syfs entries in scan_elements directories to sensor types,
 * and will also be used to determine other sysfs names as well as the iio
 * device number associated to a specific sensor.
 */

struct sensor_catalog_entry_t sensor_catalog[] = {
	DECLARE_SENSOR3("accel",      SENSOR_TYPE_ACCELEROMETER,  "x", "y", "z")
	DECLARE_SENSOR3("anglvel",    SENSOR_TYPE_GYROSCOPE,      "x", "y", "z")
	DECLARE_SENSOR3("magn",       SENSOR_TYPE_MAGNETIC_FIELD, "x", "y", "z")
	DECLARE_SENSOR0("illuminance",SENSOR_TYPE_LIGHT                        )
	DECLARE_SENSOR3("incli",      SENSOR_TYPE_ORIENTATION,    "x", "y", "z")
	DECLARE_SENSOR4("rot",        SENSOR_TYPE_ROTATION_VECTOR,
					 "quat_x", "quat_y", "quat_z", "quat_w")
	DECLARE_SENSOR0("temp",	      SENSOR_TYPE_AMBIENT_TEMPERATURE	       )
};

#define CATALOG_SIZE	ARRAY_SIZE(sensor_catalog)


/* We equate sensor handles to indices in these tables */

struct sensor_t      sensor_desc[MAX_SENSORS];	/* Android-level descriptors */
struct sensor_info_t sensor_info[MAX_SENSORS];	/* Internal descriptors      */
int sensor_count;				/* Detected sensors 	     */


static void add_sensor (int dev_num, int catalog_index, int use_polling)
{
	int s;
	int sensor_type;
	int retval;
	char sysfs_path[PATH_MAX];
	const char* prefix;
        float scale;
	int c;

	if (sensor_count == MAX_SENSORS) {
		ALOGE("Too many sensors!\n");
		return;
	}

	sensor_type = sensor_catalog[catalog_index].type;

	/*
	 * At this point we could check that the expected sysfs attributes are
	 * present ; that would enable having multiple catalog entries with the
	 * same sensor type, accomodating different sets of sysfs attributes.
	 */

	s = sensor_count;

	sensor_info[s].dev_num		= dev_num;
	sensor_info[s].catalog_index	= catalog_index;

        if (use_polling)
                sensor_info[s].num_channels = 0;
        else
                sensor_info[s].num_channels =
                                sensor_catalog[catalog_index].num_channels;

	prefix = sensor_catalog[catalog_index].tag;

	/*
	 * receiving the illumination sensor calibration inputs from
	 * the Android properties and setting it within sysfs
	 */
	if (sensor_catalog[catalog_index].type == SENSOR_TYPE_LIGHT) {
		retval = sensor_get_illumincalib(s);
		sprintf(sysfs_path, ILLUMINATION_CALIBPATH, dev_num);
		sysfs_write_int(sysfs_path, retval);
	}

	/* Read name attribute, if available */
	sprintf(sysfs_path, NAME_PATH, dev_num);
	sysfs_read_str(sysfs_path, sensor_info[s].internal_name, MAX_NAME_SIZE);

	/* See if we have general offsets and scale values for this sensor */

	sprintf(sysfs_path, SENSOR_OFFSET_PATH, dev_num, prefix);
	sysfs_read_float(sysfs_path, &sensor_info[s].offset);

	sprintf(sysfs_path, SENSOR_SCALE_PATH, dev_num, prefix);
	if (!sysfs_read_float(sysfs_path, &scale)) {
                sensor_info[s].scale = scale;
		ALOGI("Scale path %s  scale: %f, dev_num =%d \n",
                                        sysfs_path, scale, dev_num);
	}else {
                sensor_info[s].scale = 1;
                /* Read channel specific scale if any*/
                for (c = 0; c < sensor_catalog[catalog_index].num_channels; c++)
                {
                        sprintf(sysfs_path, BASE_PATH "%s", dev_num,
                                sensor_catalog[catalog_index].channel[c].scale_path);

                        if (!sysfs_read_float(sysfs_path, &scale)) {
                                sensor_info[s].channel[c].scale = scale;
			        sensor_info[s].scale = 0;
                        }
                        ALOGI("Scale path %s  channel scale: %f dev_num %d\n",
                                        sysfs_path, scale, dev_num);
                }
        }

	/* Initialize Android-visible descriptor */
	sensor_desc[s].name		= sensor_get_name(s);
	sensor_desc[s].vendor		= sensor_get_vendor(s);
	sensor_desc[s].version		= sensor_get_version(s);
	sensor_desc[s].handle		= s;
	sensor_desc[s].type		= sensor_type;
	sensor_desc[s].maxRange		= sensor_get_max_range(s);
	sensor_desc[s].resolution	= sensor_get_resolution(s);
	sensor_desc[s].power		= sensor_get_power(s);

	if (sensor_info[s].internal_name[0] == '\0') {
		/*
		 * In case the kernel-mode driver doesn't expose a name for
		 * the iio device, use (null)-dev%d as the trigger name...
		 * This can be considered a kernel-mode iio driver bug.
		 */
		ALOGW("Using null trigger on sensor %d (dev %d)\n", s, dev_num);
		strcpy(sensor_info[s].internal_name, "(null)");
	}

	/* Select one of the available sensor sample processing styles */
	select_transform(s);

	sensor_count++;
}


static void discover_poll_sensors (int dev_num, char map[CATALOG_SIZE])
{
	char base_dir[PATH_MAX];
	DIR *dir;
	char sysfs_dir[PATH_MAX];
	struct sensor *sensor;
	struct dirent *d;
	unsigned int i;
        int c;

	memset(map, 0, CATALOG_SIZE);

	snprintf(base_dir, sizeof(base_dir), BASE_PATH, dev_num);

	dir = opendir(base_dir);
	if (!dir) {
               return;
	}

	/* Enumerate entries in this iio device's base folder */

	while ((d = readdir(dir))) {
		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
			continue;

                /* If the name matches a catalog entry, flag it */
                for (i = 0; i<CATALOG_SIZE; i++)
                        for (c=0; c<sensor_catalog[i].num_channels; c++)
                                if (!strcmp(d->d_name,
				    sensor_catalog[i].channel[c].raw_path) ||
                                    !strcmp(d->d_name,
				    sensor_catalog[i].channel[c].input_path)) {
				map[i] = 1;
                                break;
                        }
	}

	closedir(dir);
}


static void discover_trig_sensors (int dev_num, char map[CATALOG_SIZE])
{
	char scan_elem_dir[PATH_MAX];
	DIR *dir;
	char sysfs_dir[PATH_MAX];
	struct sensor *sensor;
	struct dirent *d;
	unsigned int i;

        memset(map, 0, CATALOG_SIZE);

	/* Enumerate entries in this iio device's scan_elements folder */

	snprintf(scan_elem_dir, sizeof(scan_elem_dir), CHANNEL_PATH, dev_num);

	dir = opendir(scan_elem_dir);
	if (!dir) {
               return;
	}

	while ((d = readdir(dir))) {
		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
			continue;

		/* Compare en entry to known ones and create matching sensors */

                for (i = 0; i<CATALOG_SIZE; i++)
			if (!strcmp(d->d_name,
				    sensor_catalog[i].channel[0].en_path)) {
				map[i] = 1;
                                break;
                        }
	}

	closedir(dir);
}


static void orientation_sensor_check(void)
{
	/*
	 * If we have accel + gyro + magn but no rotation vector sensor,
	 * SensorService replaces the HAL provided orientation sensor by the
	 * AOSP version... provided we report one. So initialize a virtual
	 * orientation sensor with zero values, which will get replaced. See:
	 * frameworks/native/services/sensorservice/SensorService.cpp, looking
	 * for SENSOR_TYPE_ROTATION_VECTOR; that code should presumably fall
	 * back to mUserSensorList.add instead of replaceAt, but accommodate it.
	 */

	int i;
	int has_acc = 0;
	int has_gyr = 0;
	int has_mag = 0;
	int has_rot = 0;
	int has_ori = 0;

	for (i=0; i<sensor_count; i++)
		switch (sensor_catalog[sensor_info[i].catalog_index].type) {
			case SENSOR_TYPE_ACCELEROMETER:
				has_acc = 1;
				break;
			case SENSOR_TYPE_GYROSCOPE:
				has_gyr = 1;
				break;
			case SENSOR_TYPE_MAGNETIC_FIELD:
				has_mag = 1;
				break;
			case SENSOR_TYPE_ORIENTATION:
				has_ori = 1;
				break;
			case SENSOR_TYPE_ROTATION_VECTOR:
				has_rot = 1;
				break;
		}

	if (has_acc && has_gyr && has_mag && !has_rot && !has_ori)
		for (i=0; i<CATALOG_SIZE; i++)
			if (sensor_catalog[i].type == SENSOR_TYPE_ORIENTATION) {
				ALOGI("Adding placeholder orientation sensor");
				add_sensor(0, i, 1);
				break;
			}
}


void enumerate_sensors (void)
{
	/*
	 * Discover supported sensors and allocate control structures for them.
	 * Multiple sensors can potentially rely on a single iio device (each
	 * using their own channels). We can't have multiple sensors of the same
	 * type on the same device. In case of detection as both a poll-mode
	 * and trigger-based sensor, use the trigger usage mode.
	 */
	char poll_sensors[CATALOG_SIZE];
	char trig_sensors[CATALOG_SIZE];
	int dev_num;
	unsigned int i;

	for (dev_num=0; dev_num<MAX_DEVICES; dev_num++) {
		discover_poll_sensors(dev_num, poll_sensors);
		discover_trig_sensors(dev_num, trig_sensors);

		for (i=0; i<CATALOG_SIZE; i++)
			if (trig_sensors[i])
				add_sensor(dev_num, i, 0);
			else
				if (poll_sensors[i])
					add_sensor(dev_num, i, 1);
	}

	ALOGI("Discovered %d sensors\n", sensor_count);

	/* Make sure Android fall backs to its own orientation sensor */
	orientation_sensor_check();
}


void delete_enumeration_data (void)
{
	/* Reset sensor count */
	sensor_count = 0;
}


int get_sensors_list(	struct sensors_module_t* module,
			struct sensor_t const** list)
{
	*list = sensor_desc;
	return sensor_count;
}
