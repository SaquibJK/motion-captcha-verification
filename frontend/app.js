(function () {
  "use strict";

  const API_BASE = ""; // same-origin; change if serving frontend separately

  const RECORD_MS = 3000;
  const CALIBRATION_MS = 1000;

  const panels = {
    checking: document.getElementById("panel-checking"),
    insecure: document.getElementById("panel-insecure"),
    unsupported: document.getElementById("panel-unsupported"),
    permission: document.getElementById("panel-permission"),
    start: document.getElementById("panel-start"),
    calibrating: document.getElementById("panel-calibrating"),
    challenge: document.getElementById("panel-challenge"),
    result: document.getElementById("panel-result"),
  };

  function show(name) {
    Object.values(panels).forEach((p) => p.classList.add("hidden"));
    panels[name].classList.remove("hidden");
  }

  /* Modern browsers (Chrome, Safari, etc.) only expose DeviceOrientationEvent
   * / DeviceMotionEvent in secure contexts (HTTPS or localhost). Over plain
   * HTTP, these APIs are hidden entirely, which looks identical to "no
   * sensors" unless checked separately — so we check it first and report it
   * accurately instead of blaming the device. */
  function isSecureOrigin() {
    return window.isSecureContext === true;
  }

  /* UA string alone misses real phones in some cases (e.g. iPad's default
   * "desktop site" UA, some in-app browsers), so fall back to touch-capability
   * as a second signal rather than relying on the regex exclusively. */
  function looksLikeMobile() {
    const uaMatch = /Mobi|Android|iPhone|iPad|iPod/i.test(navigator.userAgent);
    const touchCapable = (navigator.maxTouchPoints || 0) > 0;
    return uaMatch || touchCapable;
  }

  function hasMotionSupport() {
    return (
      typeof window.DeviceOrientationEvent !== "undefined" &&
      typeof window.DeviceMotionEvent !== "undefined" &&
      looksLikeMobile()
    );
  }

  function needsIOSPermission() {
    return (
      typeof DeviceMotionEvent !== "undefined" &&
      typeof DeviceMotionEvent.requestPermission === "function"
    );
  }

  let currentToken = null;
  let currentChallenge = null;
  let calibration = { beta: 0, gamma: 0 };

  /* ---------------- calibration phase ---------------- */

  /* Reads orientation/acceleration for ~1s without asking the user to move,
   * to establish the phone's starting position (spec section 3). Since
   * users hold phones differently, no fixed starting orientation is
   * assumed — this baseline is what later measurements are compared to. */
  function calibrate() {
    return new Promise((resolve) => {
      let betaSum = 0, gammaSum = 0, count = 0;

      function onOrientation(e) {
        betaSum += e.beta || 0;
        gammaSum += e.gamma || 0;
        count++;
      }

      window.addEventListener("deviceorientation", onOrientation);

      setTimeout(() => {
        window.removeEventListener("deviceorientation", onOrientation);
        resolve({
          beta: count ? betaSum / count : 0,
          gamma: count ? gammaSum / count : 0,
        });
      }, CALIBRATION_MS);
    });
  }

  async function fetchChallenge() {
    const res = await fetch(API_BASE + "/api/challenge");
    if (!res.ok) throw new Error("Failed to fetch challenge");
    const data = await res.json();
    currentToken = data.token;
    currentChallenge = data.challenge;
    document.getElementById("challenge-instruction").textContent = data.instruction;
  }

  async function submitVerify(samples) {
    const res = await fetch(API_BASE + "/api/verify", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        token: currentToken,
        samples: samples,
        calibration: calibration,
      }),
    });
    return res.json();
  }

  function setRing(fraction, label) {
    const circumference = 326.7;
    const progress = document.getElementById("ring-progress");
    progress.style.strokeDashoffset = String(circumference * (1 - fraction));
    document.getElementById("ring-label").textContent = label;
  }

  function recordMotion() {
    return new Promise((resolve) => {
      const samples = [];
      const readout = document.getElementById("live-readout");
      readout.classList.remove("hidden");
      readout.textContent = "";

      let latestOrientation = { alpha: 0, beta: 0, gamma: 0 };
      const start = performance.now();

      function onOrientation(e) {
        latestOrientation = {
          alpha: e.alpha || 0,
          beta: e.beta || 0,
          gamma: e.gamma || 0,
        };
      }

      function onMotion(e) {
        const t = performance.now() - start;
        const acc = e.accelerationIncludingGravity || { x: 0, y: 0, z: 9.81 };
        const sample = {
          t: t,
          alpha: latestOrientation.alpha,
          beta: latestOrientation.beta,
          gamma: latestOrientation.gamma,
          ax: acc.x || 0,
          ay: acc.y || 0,
          az: acc.z || 9.81,
        };
        samples.push(sample);
        if (samples.length % 4 === 0) {
          readout.textContent =
            `samples: ${samples.length}\n` +
            `alpha ${sample.alpha.toFixed(1)}  beta ${sample.beta.toFixed(1)}  gamma ${sample.gamma.toFixed(1)}\n` +
            `ax ${sample.ax.toFixed(1)}  ay ${sample.ay.toFixed(1)}  az ${sample.az.toFixed(1)}`;
        }
        setRing(Math.min(1, t / RECORD_MS), `${Math.max(0, ((RECORD_MS - t) / 1000)).toFixed(1)}s`);
      }

      window.addEventListener("deviceorientation", onOrientation);
      window.addEventListener("devicemotion", onMotion);

      setTimeout(() => {
        window.removeEventListener("deviceorientation", onOrientation);
        window.removeEventListener("devicemotion", onMotion);
        resolve(samples);
      }, RECORD_MS);
    });
  }

  function showResult(result) {
    const icon = document.getElementById("result-icon");
    const title = document.getElementById("result-title");
    const reason = document.getElementById("result-reason");
    const confidence = document.getElementById("meta-confidence");
    const time = document.getElementById("meta-time");

    if (result.success) {
      icon.textContent = "✓";
      icon.style.color = "var(--success)";
      title.textContent = "Verified";
    } else {
      icon.textContent = "✕";
      icon.style.color = "var(--danger)";
      title.textContent = result.error === "invalid_or_expired_token"
        ? "Challenge expired"
        : "Not verified";
    }
    reason.textContent = result.reason || (result.error ? "Please request a new challenge and try again." : "");
    confidence.textContent = typeof result.confidence === "number" ? `${Math.round(result.confidence)}%` : "—";
    time.textContent = typeof result.duration_ms === "number" ? `${(result.duration_ms / 1000).toFixed(1)}s` : "—";

    show("result");
  }

  async function runChallengeFlow() {
    show("calibrating");
    calibration = await calibrate();

    show("checking");
    try {
      await fetchChallenge();
    } catch (e) {
      panels.checking.querySelector(".status").textContent =
        "Could not reach the verification server.";
      return;
    }
    show("challenge");
    setRing(0, "Ready");
  }

  async function startRecording() {
    document.getElementById("btn-start").disabled = true;
    setRing(0, "Go!");
    const samples = await recordMotion();
    document.getElementById("btn-start").disabled = false;
    document.getElementById("live-readout").classList.add("hidden");

    if (samples.length === 0) {
      showResult({ success: false, reason: "No sensor data was captured. Check browser permissions." });
      return;
    }
    try {
      const result = await submitVerify(samples);
      showResult(result);
    } catch (e) {
      showResult({ success: false, reason: "Could not reach the verification server." });
    }
  }

  /* ---------------- wiring ---------------- */

  document.getElementById("btn-permission").addEventListener("click", async () => {
    try {
      const result = await DeviceMotionEvent.requestPermission();
      if (result === "granted") {
        show("start");
      } else {
        panels.permission.querySelector(".status").textContent =
          "Permission denied. Enable motion access in browser settings.";
      }
    } catch (e) {
      panels.permission.querySelector(".status").textContent =
        "Could not request motion permission.";
    }
  });

  document.getElementById("btn-start-verification").addEventListener("click", runChallengeFlow);
  document.getElementById("btn-start").addEventListener("click", startRecording);
  document.getElementById("btn-retry").addEventListener("click", runChallengeFlow);

  /* ---------------- init ---------------- */

  (function init() {
    show("checking");
    if (!isSecureOrigin() && looksLikeMobile()) {
      show("insecure");
      return;
    }
    if (!hasMotionSupport()) {
      show("unsupported");
      return;
    }
    if (needsIOSPermission()) {
      show("permission");
      return;
    }
    show("start");
  })();
})();
