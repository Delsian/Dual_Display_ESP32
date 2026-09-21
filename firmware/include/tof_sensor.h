/**
 * @file tof_sensor.h
 * @author Intellar (https://github.com/intellar)
 * @brief Header file for the VL53L5CX Time-of-Flight sensor control.
 * @version 1.0
 *
 * @copyright Copyright (c) 2025
 *
 * @license See LICENSE.md for details.
 *
 */
#ifndef TOF_SENSOR_H
#define TOF_SENSOR_H

#include <Arduino.h>
#include "config.h"
#include <Wire.h>

// Library for the VL53L5CX distance sensor
// Install it through the library manager: "SparkFun VL53L5CX"
#include <SparkFun_VL53L5CX_Library.h>


// Structure storing the position of the target detected by the ToF sensor
struct TofTarget {
    float x; // Horizontal position (-1.0 to 1.0)
    float y; // Vertical position (-1.0 to 1.0)
    int distance_mm; // Distance in mm
    bool is_valid; // Whether a target has been detected
    int8_t min_dist_pixel_x; // X coordinate of the nearest pixel (for debugging)
    int8_t min_dist_pixel_y; // Y coordinate of the nearest pixel (for debugging)
    long match_score; // Template matching correlation score (for debugging)
};

// Initializes the ToF sensor. Must be called in setup().
void init_tof_sensor();

// Reads sensor data and updates the target structure.
void update_tof_sensor_data();

// Returns the last detected target position.
TofTarget get_tof_target();

// Returns a pointer to the raw sensor measurement data.
const VL53L5CX_ResultsData* get_tof_measurement_data();

#endif // TOF_SENSOR_H
