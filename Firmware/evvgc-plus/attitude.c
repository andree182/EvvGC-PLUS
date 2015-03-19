/*
    EvvGC-PLUS - Copyright (C) 2013-2015 Sarunas Vaitekonis

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

/*
  References:
  [1] Mahony, R.; Hamel, T.; Pflimlin, Jean-Michel, "Nonlinear Complementary
      Filters on the Special Orthogonal Group", Automatic Control,
      IEEE Transactions on, vol.53, no.5, pp.1203,1218, June 2008
  [2] Euston, M.; Coote, P.; Mahony, R.; Jonghyuk Kim; Hamel, T.,
      "A complementary filter for attitude estimation of a fixed-wing UAV",
      Intelligent Robots and Systems, 2008. IROS 2008. IEEE/RSJ International
      Conference on, vol., no., pp.340,345, 22-26 Sept. 2008
*/

/*
  Note:
  - the order of rotation of the standard 3D-gimbal system is:
    Pitch (X) then Roll (Y) and then Yaw (Z).
*/

/* C libraries: */
#include <string.h>

#include "misc.h"
#include "attitude.h"
#include "pwmio.h"

#define FIXED_DT_STEP             0.002f

#define MOTOR_STEP_LIMIT_MAX      M_PI / 45.0f
#define MOTOR_STEP_LIMIT_MIN      -MOTOR_STEP_LIMIT_MAX

#define ACCEL_TAU                 0.1f
#define INPUT_SIGNAL_ALPHA        266.7f
#define MODE_FOLLOW_DEAD_BAND     M_PI / 36.0f

/* PID controller structure. */
typedef struct tagPIDStruct {
  float P;
  float I;
  float D;
  float F;
  float prevDistance;
  float prevSpeed;
  float prevDisturbance;
  float prevCommand;
} __attribute__((packed)) PIDStruct, *PPIDStruct;

/**
 * Global variables.
 */
/* Mechanical offset of the motors. */
float g_motorOffset[3] = {0.0f, 0.0f, 0.0f};

/**
 * Default closed loop with feed forward settings.
 */
PIDSettings g_pidSettings[3] = {
/* P, I, D, F */
  {0, 0, 0, 0}, /* Pitch PID */
  {0, 0, 0, 0}, /* Roll  PID */
  {0, 0, 0, 0}, /* Yaw   PID */
};

/**
 * Default input mode settings.
 */
InputModeStruct g_modeSettings[3] = {
  {-60,               /* Min angle */
   60,                /* Max angle */
   0,                 /* Offset    */
   20,                /* Speed     */
   INPUT_MODE_ANGLE}, /* Mode ID   */
  {-60,               /* Min angle */
   60,                /* Max angle */
   0,                 /* Offset    */
   20,                /* Speed     */
   INPUT_MODE_ANGLE}, /* Mode ID   */
  {-90,               /* Min angle */
   90,                /* Max angle */
   0,                 /* Offset    */
   20,                /* Speed     */
   INPUT_MODE_SPEED}, /* Mode ID   */
};

/**
 * Local variables
 */
static float camRot[3] = {0.0f};
static float camRotSpeedPrev[3] = {0.0f};

static float accelKp = 0.02f;
static float accelKi = 0.0002;

/* Accelerometer filter variables. */
static uint8_t fAccelFilterEnabled = TRUE;
static float accel_alpha = 0.0f;

/* PID controller parameters. */
static PIDStruct PID[3] = {
  {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
  {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
  {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}
};

/**
 * @brief  Implements basic PID stabilization of the motor speed with feed forward.
 * @param  cmd_id - command id to apply PID action to.
 * @param  sp - set-point value.
 * @param  pv - process variable value.
 * @param  d - disturbance value from the 2nd IMU.
 * @return weighted sum of P, I and D actions.
 */
static float pidControllerApply(uint8_t cmd_id, float sp, float pv, float d) {
  float poles2 = (float)(g_pwmOutput[cmd_id].num_poles >> 1);
  /* Distance for the motor to travel: */
  float distance = circadjust(sp - pv, M_PI);
  /* Convert mechanical distance to electrical distance: */
  distance *= poles2;
  /* If there is a distance to travel then rotate the motor in small steps: */
  float step = constrain(distance*PID[cmd_id].I, MOTOR_STEP_LIMIT_MIN, MOTOR_STEP_LIMIT_MAX);
  /* Calculate proportional speed of the motor: */
  float speed = distance - PID[cmd_id].prevDistance;
  step += speed*PID[cmd_id].P;
  /* Account for the acceleration of the motor: */
  step += (speed - PID[cmd_id].prevSpeed)*PID[cmd_id].D;
  /* Account for the disturbance value (only if the 2nd IMU is enabled): */
  step += (PID[cmd_id].prevDisturbance - d)*PID[cmd_id].F*poles2;
  /* Update offset of the motor: */
  g_motorOffset[cmd_id] += step / poles2;
  /* Wind-up guard limits motor offset range to one mechanical rotation: */
  g_motorOffset[cmd_id] = circadjust(g_motorOffset[cmd_id], M_PI);
  /* Update motor position: */
  float cmd = PID[cmd_id].prevCommand + step;
  /* Normalize command to -M_PI..M_PI range: */
  if (cmd < 0.0f) {
    cmd = fmodf(cmd - M_PI, -M_TWOPI) + M_PI;
  } else {
    cmd = fmodf(cmd + M_PI,  M_TWOPI) - M_PI;
  }
  /* Save values for the next iteration: */
  PID[cmd_id].prevDistance    = distance;
  PID[cmd_id].prevSpeed       = speed;
  PID[cmd_id].prevDisturbance = d;
  PID[cmd_id].prevCommand     = cmd;
  return cmd;
}

/**
 * @brief
 */
static void pidUpdateStruct(void) {
  uint8_t i;
  for (i = 0; i < 3; i++) {
    PID[i].P = (float)g_pidSettings[i].P*0.1f;
    PID[i].I = (float)g_pidSettings[i].I*0.01f;
    PID[i].D = (float)g_pidSettings[i].D*1.0f;
    PID[i].F = (float)g_pidSettings[i].F*0.01f;
    if (!g_pidSettings[i].I) {
      g_motorOffset[i] = 0.0f;
    }
  }
}

/**
 * @brief  First order low-pass filter.
 * @param  raw - pointer to raw data array;
 * @param  filtered - pointer to filtered data array;
 */
static void accelFilterApply(const float raw[], float filtered[]) {
  if (fAccelFilterEnabled) {
    filtered[0] = (filtered[0] - raw[0])*accel_alpha + raw[0];
    filtered[1] = (filtered[1] - raw[1])*accel_alpha + raw[1];
    filtered[2] = (filtered[2] - raw[2])*accel_alpha + raw[2];
  } else {
    memcpy((void *)filtered, (void *)raw, sizeof(float)*3);
  }
}

/**
 * @brief
 */
void attitudeInit(void) {
  memset((void *)PID, 0, sizeof(PID));
  pidUpdateStruct();
  accel_alpha = expf(-FIXED_DT_STEP / ACCEL_TAU);
}

/**
 * @brief
 */
void attitudeUpdate(PIMUStruct pIMU) {
  float accelErr[3] = {0.0f};
  float mag;
  float dq[4];

  pIMU->accelData[0] -= pIMU->accelBias[0];
  pIMU->accelData[1] -= pIMU->accelBias[1];
  pIMU->accelData[2] -= pIMU->accelBias[2];

  pIMU->gyroData[0] -= pIMU->gyroBias[0];
  pIMU->gyroData[1] -= pIMU->gyroBias[1];
  pIMU->gyroData[2] -= pIMU->gyroBias[2];

  // Account for accel's magnitude.
  mag = QInvSqrtf(pIMU->accelData[0]*pIMU->accelData[0] + pIMU->accelData[1]*pIMU->accelData[1] + pIMU->accelData[2]*pIMU->accelData[2]);

  if ((mag > 0.0724f) && (mag < 0.1724f)) {
    float grot[3];

    // Rotate gravity to body frame and cross with accels.
    grot[0] = -(2.0f*(pIMU->qIMU[1]*pIMU->qIMU[3] - pIMU->qIMU[0]*pIMU->qIMU[2]));
    grot[1] = -(2.0f*(pIMU->qIMU[2]*pIMU->qIMU[3] + pIMU->qIMU[0]*pIMU->qIMU[1]));
    grot[2] =  (2.0f*(pIMU->qIMU[1]*pIMU->qIMU[1] + pIMU->qIMU[2]*pIMU->qIMU[2])) - 1.0f;

    // Apply smoothing to accel values, to reduce vibration noise before main calculations.
    accelFilterApply(pIMU->accelData, pIMU->accelFiltered);
    // Apply the same filtering to the rotated attitude to match phase shift.
    accelFilterApply(grot, pIMU->grotFiltered);
    // Compute the error between the predicted direction of gravity and smoothed acceleration.
    CrossProduct(pIMU->accelFiltered, pIMU->grotFiltered, accelErr);

    // Normalize accel_error.
    accelErr[0] *= mag;
    accelErr[1] *= mag;
    accelErr[2] *= mag;
  }

  // Correct rates based on error.
  pIMU->gyroData[0] += accelErr[0]*(accelKp / FIXED_DT_STEP);
  pIMU->gyroData[1] += accelErr[1]*(accelKp / FIXED_DT_STEP);
  pIMU->gyroData[2] += accelErr[2]*(accelKp / FIXED_DT_STEP);

  // Correct rates based on error.
  pIMU->gyroBias[0] -= accelErr[0]*accelKi;
  pIMU->gyroBias[1] -= accelErr[1]*accelKi;
  pIMU->gyroBias[2] -= accelErr[2]*accelKi;

  dq[0] = (-pIMU->qIMU[1]*pIMU->gyroData[0] - pIMU->qIMU[2]*pIMU->gyroData[1] - pIMU->qIMU[3]*pIMU->gyroData[2])*(FIXED_DT_STEP*DEG2RAD*0.5f);
  dq[1] = ( pIMU->qIMU[0]*pIMU->gyroData[0] - pIMU->qIMU[3]*pIMU->gyroData[1] + pIMU->qIMU[2]*pIMU->gyroData[2])*(FIXED_DT_STEP*DEG2RAD*0.5f);
  dq[2] = ( pIMU->qIMU[3]*pIMU->gyroData[0] + pIMU->qIMU[0]*pIMU->gyroData[1] - pIMU->qIMU[1]*pIMU->gyroData[2])*(FIXED_DT_STEP*DEG2RAD*0.5f);
  dq[3] = (-pIMU->qIMU[2]*pIMU->gyroData[0] + pIMU->qIMU[1]*pIMU->gyroData[1] + pIMU->qIMU[0]*pIMU->gyroData[2])*(FIXED_DT_STEP*DEG2RAD*0.5f);

  pIMU->qIMU[0] += dq[0];
  pIMU->qIMU[1] += dq[1];
  pIMU->qIMU[2] += dq[2];
  pIMU->qIMU[3] += dq[3];

  // Normalize attitude quaternion.
  mag = QInvSqrtf(pIMU->qIMU[0]*pIMU->qIMU[0] + pIMU->qIMU[1]*pIMU->qIMU[1] + pIMU->qIMU[2]*pIMU->qIMU[2] + pIMU->qIMU[3]*pIMU->qIMU[3]);
  pIMU->qIMU[0] *= mag;
  pIMU->qIMU[1] *= mag;
  pIMU->qIMU[2] *= mag;
  pIMU->qIMU[3] *= mag;

  // Convert attitude into Euler angles;
  Quaternion2RPY(pIMU->qIMU, pIMU->rpy);
}

/**
 * @brief
 */
void cameraRotationUpdate(void) {
  uint8_t i;
  float coef;
  float speedLimit;

  for (i = 0; i < 3; i++) {
    speedLimit = ((float)g_modeSettings[i].speed)*DEG2RAD;

    if (g_modeSettings[i].mode_id & INPUT_MODE_FOLLOW) {
      /* Calculate offset of the gimbal: */
      coef = g_modeSettings[i].offset*DEG2RAD - g_motorOffset[i];
      if (coef > MODE_FOLLOW_DEAD_BAND) {
        coef -= MODE_FOLLOW_DEAD_BAND;
        /* Convert to speed: */
        coef /= INPUT_SIGNAL_ALPHA*FIXED_DT_STEP;
      } else if (coef < -MODE_FOLLOW_DEAD_BAND) {
        coef += MODE_FOLLOW_DEAD_BAND;
        /* Convert to speed: */
        coef /= INPUT_SIGNAL_ALPHA*FIXED_DT_STEP;
      } else {
        coef = 0.0f;
      }
    } else if (g_mixedInput[i].channel_id == INPUT_CHANNEL_DISABLED) {
      camRot[i] = 0.0f;
      continue;
    } else {
      /* Calculate input scaling coefficient: */
      if (g_mixedInput[i].max_val == g_mixedInput[i].min_val) {
        /* Avoid divisions by zero. */
        coef = 0.0f;
      } else {
        coef = ((float)(g_inputValues[g_mixedInput[i].channel_id] - g_mixedInput[i].mid_val)) /
               ((float)(g_mixedInput[i].max_val - g_mixedInput[i].min_val));
      }

      if (g_modeSettings[i].mode_id & INPUT_MODE_SPEED) {
        /* Calculate speed from RC input data: */
        coef *= 2.0f*speedLimit;
        camRotSpeedPrev[i] += (coef - camRotSpeedPrev[i]) / INPUT_SIGNAL_ALPHA;
        coef = camRotSpeedPrev[i];
      } else { /* INPUT_MODE_ANGLE */
        /* Calculate angle from input data: */
        coef *= (g_modeSettings[i].max_angle - g_modeSettings[i].min_angle);
        coef += (g_modeSettings[i].max_angle + g_modeSettings[i].min_angle) / 2;
        coef = constrain(coef, (float)g_modeSettings[i].min_angle, (float)g_modeSettings[i].max_angle);
        coef *= DEG2RAD;

        /* Convert angle difference to speed: */
        coef = (coef - camRot[i]) / INPUT_SIGNAL_ALPHA / FIXED_DT_STEP;
      }
    }
    coef = constrain(coef, -speedLimit, speedLimit);
    camRot[i] += coef*FIXED_DT_STEP;
    camRot[i] = circadjust(camRot[i], M_PI);
  }
}

/**
 * @brief
 */
void actuatorsUpdate(void) {
  float cmd = 0.0f;
  /* Pitch: */
  uint8_t cmd_id = g_pwmOutput[PWM_OUT_PITCH].dt_cmd_id & PWM_OUT_CMD_ID_MASK;
  if (cmd_id != PWM_OUT_CMD_DISABLED) {
    cmd = pidControllerApply(cmd_id, camRot[cmd_id], g_IMU1.rpy[cmd_id], g_IMU2.rpy[cmd_id]);
  }
  pwmOutputUpdate(PWM_OUT_PITCH, cmd);
  cmd = 0.0f;
  /* Roll: */
  cmd_id = g_pwmOutput[PWM_OUT_ROLL].dt_cmd_id & PWM_OUT_CMD_ID_MASK;
  if (cmd_id != PWM_OUT_CMD_DISABLED) {
    cmd = pidControllerApply(cmd_id, camRot[cmd_id], g_IMU1.rpy[cmd_id], g_IMU2.rpy[cmd_id]);
  }
  pwmOutputUpdate(PWM_OUT_ROLL, cmd);
  cmd = 0.0f;
  /* Yaw: */
  cmd_id = g_pwmOutput[PWM_OUT_YAW].dt_cmd_id & PWM_OUT_CMD_ID_MASK;
  if (cmd_id != PWM_OUT_CMD_DISABLED) {
    cmd = pidControllerApply(cmd_id, camRot[cmd_id], g_IMU1.rpy[cmd_id], g_IMU2.rpy[cmd_id]);
  }
  pwmOutputUpdate(PWM_OUT_YAW, cmd);
}

/**
 * @brief
 */
void pidSettingsUpdate(const PPIDSettings pNewSettings) {
  memcpy((void *)&g_pidSettings, (void *)pNewSettings, sizeof(g_pidSettings));
  pidUpdateStruct();
}

/**
 * @brief
 */
void inputModeSettingsUpdate(const PInputModeStruct pNewSettings) {
  memcpy((void *)&g_modeSettings, (void *)pNewSettings, sizeof(g_modeSettings));
}
