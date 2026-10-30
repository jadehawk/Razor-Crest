#include "HalTiltSensor.h"

#include <Logging.h>

HalTiltSensor halTiltSensor;

void HalTiltSensor::begin() {
  if (!_sdkImu.begin()) {
    LOG_ERR("GYR", "SDK IMU not found");
    return;
  }
  // Leave the QMI8658 in real power-down until an active reader needs it.
  if (!_sdkImu.sleep()) {
    LOG_ERR("GYR", "IMU standby failed");
    _isAwake = true;
  }
  _available = _sdkImu.hasGyroscope();
  if (!_available) {
    LOG_INF("GYR", "IMU has no gyroscope");
    return;
  }
  _initMs = millis();
  _lastPollMs = millis();
  LOG_INF("GYR", "SDK IMU initialized");
}

bool HalTiltSensor::wake() {
  if (!_available) return false;
  if (!_sdkImu.wake()) {
    _sdkImu.sleep();
    LOG_ERR("GYR", "IMU wake failed");
    return false;
  }
  _lastPollMs = millis();
  _lastTiltMs = millis();
  _wakeMs = millis();
  _isAwake = true;
  _orientationDetector.reset();
  _pendingAutoOrientation = -1;
  return true;
}

bool HalTiltSensor::deepSleep() {
  if (!_available) return false;
  if (!_sdkImu.sleep()) {
    LOG_ERR("GYR", "IMU sleep failed");
    return false;
  }
  clearPendingEvents();
  _inTilt = false;
  _isAwake = false;
  _orientationDetector.reset();
  _pendingAutoOrientation = -1;
  return true;
}

void HalTiltSensor::update(const uint8_t mode, const uint8_t orientation, const bool inReader, const bool autoRotate) {
  if (!_available) return;

  // A background or stacked reader must not hold the IMU awake.
  const bool needed = inReader && (mode != CrossPointTiltPageTurn::TILT_OFF || autoRotate);
  if (!needed) {
    _orientationDetector.reset();
    _pendingAutoOrientation = -1;
  }
  if (needed != _isAwake) {
    const unsigned long attemptAt = millis();
    if (_powerAttemptFailed && attemptAt - _lastPowerAttemptMs < 1000) return;
    _lastPowerAttemptMs = attemptAt;
    _powerAttemptFailed = !(needed ? wake() : deepSleep());
    return;
  }
  _powerAttemptFailed = false;
  if (!needed) return;

  const unsigned long now = millis();
  if ((now - _wakeMs) < WAKE_STABILIZE_MS || (now - _lastPollMs) < POLL_INTERVAL_MS) return;
  _lastPollMs = now;

  // One SDK sample contains acceleration and angular rate; never read twice.
  Imu::Sample sample{};
  if (!_sdkImu.read(sample)) return;

  if (autoRotate) {
    const int8_t next = _orientationDetector.update(sample.ax, sample.ay, sample.az, orientation, now);
    if (next >= 0) {
      _pendingAutoOrientation = next;
      _hadActivity = true;
    }
  } else {
    _orientationDetector.reset();
    _pendingAutoOrientation = -1;
  }

  if (mode == CrossPointTiltPageTurn::TILT_OFF) {
    _inTilt = false;
    return;
  }

  // On X3, gyro X turns pages in portrait and gyro Y in landscape.
  float tiltAxis;
  switch (orientation) {
    case CrossPointOrientation::PORTRAIT:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? -sample.gx : sample.gx;
      break;
    case CrossPointOrientation::INVERTED:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? sample.gx : -sample.gx;
      break;
    case CrossPointOrientation::LANDSCAPE_CW:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? sample.gy : -sample.gy;
      break;
    case CrossPointOrientation::LANDSCAPE_CCW:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? -sample.gy : sample.gy;
      break;
    default:
      tiltAxis = sample.gx;
      break;
  }

  if (_inTilt) {
    if (fabsf(tiltAxis) < NEUTRAL_RATE_DPS) _inTilt = false;
  } else if (now - _lastTiltMs >= COOLDOWN_MS) {
    if (tiltAxis > RATE_THRESHOLD_DPS) {
      _tiltForwardEvent = true;
      _hadActivity = true;
      _inTilt = true;
      _lastTiltMs = now;
      LOG_INF("GYR", "Forward Trigger=(%.1f) dps", tiltAxis);
    } else if (tiltAxis < -RATE_THRESHOLD_DPS) {
      _tiltBackEvent = true;
      _hadActivity = true;
      _inTilt = true;
      _lastTiltMs = now;
      LOG_INF("GYR", "Backward Trigger=(%.1f) dps", tiltAxis);
    }
  }
}

int8_t HalTiltSensor::takeAutoOrientation() {
  const int8_t direction = _pendingAutoOrientation;
  _pendingAutoOrientation = -1;
  return direction;
}

bool HalTiltSensor::wasTiltedForward() {
  const bool val = _tiltForwardEvent;
  _tiltForwardEvent = false;
  return val;
}

bool HalTiltSensor::wasTiltedBack() {
  const bool val = _tiltBackEvent;
  _tiltBackEvent = false;
  return val;
}

bool HalTiltSensor::hadActivity() {
  const bool val = _hadActivity;
  _hadActivity = false;
  return val;
}

void HalTiltSensor::clearPendingEvents() {
  _tiltForwardEvent = false;
  _tiltBackEvent = false;
  _hadActivity = false;
  // A held tilt cannot trigger again until the device returns to neutral.
}
