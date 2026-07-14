# Motion CAPTCHA Verification

A browser-based human verification system that uses **smartphone motion sensors** instead of traditional image or text CAPTCHAs.

The project captures motion data using the **DeviceMotion** and **DeviceOrientation** browser APIs, sends the recorded sensor data to a lightweight **C backend**, and verifies whether the user performed the requested gesture correctly using confidence-based motion analysis.

---

## Motivation

Traditional CAPTCHA systems are becoming increasingly frustrating for users while also becoming easier for automated systems to solve.

Since modern smartphones already contain accurate motion sensors, this project explores whether **natural physical gestures** can be used as an alternative method of proving human presence.

Instead of selecting images or typing distorted text, users simply perform a short motion challenge such as:

- Shake the phone twice
- Turn the phone upside down
- Quickly lift the phone up
- Quickly lower the phone down

The backend analyzes the recorded sensor data and returns a verification result with a confidence score.

---

# System Overview

<p align="center">
<img src="docs/images/system-overview.png" width="100%">
</p>

The verification process consists of the following stages:

1. The user opens the webpage.
2. Browser permission is requested for motion sensors.
3. The phone is calibrated to establish a baseline orientation.
4. The client requests a motion challenge from the server.
5. The server generates a random challenge and verification token.
6. The user performs the requested gesture.
7. Motion sensor data is collected in the browser.
8. Sensor data and the verification token are sent to the backend.
9. The C verification engine evaluates the motion.
10. The backend returns a pass/fail result with a confidence score.

---

# Implemented Motion Challenges

| Challenge | API Used | Verification Method |
|------------|----------|---------------------|
| Shake Twice | DeviceMotion API | Detects two distinct acceleration peaks |
| Turn Upside Down | DeviceOrientation API | Measures change in Beta angle |
| Quick Lift Up | DeviceMotion API | Detects upward acceleration ("elevator effect") |
| Quick Lower Down | DeviceMotion API | Detects downward acceleration ("elevator effect") |

---

# How It Works

### 1. Motion Recording

The browser records motion data using:

- DeviceMotion API
- DeviceOrientation API

Each sample includes timestamped sensor readings which are stored until the challenge is completed.

---

### 2. Data Transmission

The collected samples are sent to the backend as JSON together with the verification token.

The server validates the token before beginning motion verification.

---

### 3. Motion Verification

Depending on the requested challenge, different algorithms are used.

#### Shake Twice

The verifier computes the overall acceleration magnitude

```
√(ax² + ay² + az²)
```

Two distinct acceleration peaks above a predefined threshold must be detected.

---

#### Turn Upside Down

The verifier compares the calibrated **Beta** angle with the recorded Beta values.

A sufficient change in orientation indicates that the phone has been turned upside down.

---

#### Quick Lift Up

The verifier analyses acceleration along the vertical axis.

A quick lift produces the characteristic sequence:

```
Positive acceleration
↓
Stopping acceleration
```

---

#### Quick Lower Down

The opposite sequence is expected:

```
Negative acceleration
↓
Stopping acceleration
```

---

### 4. Confidence Score

Each challenge is evaluated using multiple criteria.

Depending on the gesture, the verifier considers:

- Motion strength
- Correct movement sequence
- Motion smoothness
- Timing
- Rotation amount

Each component contributes to the final confidence score.

The verification succeeds only if the confidence exceeds the required threshold.

---

# Project Structure

```
motion-captcha-verification/

├── backend/
│ ├── server.c
│ ├── verify.c
│ ├── challenge.c
│ ├── logging.c
│ └── ...
│
├── frontend/
│ ├── index.html
│ ├── script.js
│ ├── style.css
│ └── ...
│
├── docs/
│ └── images/
│ └── system-overview.png
│
├── README.md
```

---


# Current Limitations

This project is intended as a proof of concept.

Current limitations include:

- Motion challenges may be difficult for users with certain motor impairments.
- Sensor spoofing remains an active research challenge.
- Gesture thresholds currently use manually selected values.

---

# Future Work

Possible improvements include:

- Adaptive thresholds for different users
- Machine learning based gesture verification
- Continuous authentication
- Smartwatch and wearable support
- Multi-device verification
- Improved resistance against sensor spoofing
