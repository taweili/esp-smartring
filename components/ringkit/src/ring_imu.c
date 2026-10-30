/*
 * ringkit — IMU. See ring_imu.h.
 */
#include "ringkit/ring_imu.h"

#include "bsp/smartring_plus.h"

void ring_imu_accel(float *ax, float *ay, float *az)
{
    bsp_imu_get_accel(ax, ay, az);
}

void ring_imu_tilt(float *gx, float *gy)
{
    bsp_imu_get_tilt(gx, gy);
}

void ring_imu_calib_start(void)
{
    bsp_imu_calib_start();
}

ring_imu_calib_t ring_imu_calib_poll(void)
{
    switch (bsp_imu_calib_poll()) {
    case BSP_IMU_CALIB_RUNNING: return RING_IMU_CALIB_RUNNING;
    case BSP_IMU_CALIB_PASSED:  return RING_IMU_CALIB_PASSED;
    case BSP_IMU_CALIB_FAILED:  return RING_IMU_CALIB_FAILED;
    default:                    return RING_IMU_CALIB_IDLE;
    }
}

void ring_imu_calib_info(float *std_g, float *tilt_deg, uint32_t *elapsed_ms)
{
    bsp_imu_calib_get_info(std_g, tilt_deg, elapsed_ms);
}
