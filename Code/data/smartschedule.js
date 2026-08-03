/*
 * SmartAndRelax – Smart Schedule user interface
 * Independent implementation for the SmartAndRelax HTTP API.
 *
 * Expected endpoints:
 *   POST /getsmartschedule/
 *   POST /setsmartschedule/
 *   POST /updatesmartschedule/
 *   POST /cancelsmartschedule/
 *   POST /getconfig/
 */
(function () {
  "use strict";

  const POLL_INTERVAL_MS = 5000;
  const API = Object.freeze({
    status: "/getsmartschedule/",
    create: "/setsmartschedule/",
    update: "/updatesmartschedule/",
    cancel: "/cancelsmartschedule/",
    config: "/getconfig/",
  });

  let pollHandle = 0;
  let requestRunning = false;

  function byId(id) {
    return document.getElementById(id);
  }

  function setText(id, value) {
    const node = byId(id);
    if (node) node.textContent = value;
  }

  function setHtml(id, value) {
    const node = byId(id);
    if (node) node.innerHTML = value;
  }

  function show(id, visible, displayMode) {
    const node = byId(id);
    if (node) node.style.display = visible ? (displayMode || "block") : "none";
  }

  function numeric(value, fallback) {
    const result = Number(value);
    return Number.isFinite(result) ? result : fallback;
  }

  async function postJson(url, payload) {
    const response = await fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload || {}),
      cache: "no-store",
    });

    if (!response.ok) {
      const message = (await response.text()).trim();
      throw new Error(message || ("HTTP " + response.status));
    }

    const type = response.headers.get("content-type") || "";
    return type.indexOf("application/json") >= 0
      ? response.json()
      : response.text();
  }

  function twoDigits(value) {
    return String(value).padStart(2, "0");
  }

  function localDateTimeInput(date) {
    return (
      date.getFullYear() +
      "-" + twoDigits(date.getMonth() + 1) +
      "-" + twoDigits(date.getDate()) +
      "T" + twoDigits(date.getHours()) +
      ":" + twoDigits(date.getMinutes())
    );
  }

  function germanDateTime(unixSeconds) {
    const timestamp = numeric(unixSeconds, 0);
    if (timestamp <= 0) return "--";
    return new Date(timestamp * 1000).toLocaleString("de-DE", {
      day: "2-digit",
      month: "2-digit",
      year: "numeric",
      hour: "2-digit",
      minute: "2-digit",
    });
  }

  function duration(seconds) {
    let remaining = Math.max(0, Math.round(numeric(seconds, 0)));
    const days = Math.floor(remaining / 86400);
    remaining %= 86400;
    const hours = Math.floor(remaining / 3600);
    remaining %= 3600;
    const minutes = Math.floor(remaining / 60);
    const secs = remaining % 60;

    const parts = [];
    if (days) parts.push(days + (days === 1 ? " Tag" : " Tage"));
    if (hours || days) parts.push(hours + " Std.");
    parts.push(minutes + " Min.");
    if (!days && !hours && minutes < 5) parts.push(secs + " Sek.");
    return parts.join(" ");
  }

  function countdown(value) {
    const seconds = numeric(value, 0);
    return seconds > 0 ? duration(seconds) : "Jetzt";
  }

  function initializeFormDefaults() {
    const target = byId("targetDateTime");
    if (target && !target.value) {
      const next = new Date();
      next.setHours(19, 0, 0, 0);
      if (next.getTime() <= Date.now()) next.setDate(next.getDate() + 1);
      target.value = localDateTimeInput(next);
      target.min = localDateTimeInput(new Date(Date.now() + 60000));
    }
  }

  function updateInactiveForm(status) {
    const targetTemp = numeric(status.GLOBALTARGET, 0);
    if (targetTemp >= 20 && targetTemp <= 40) {
      const input = byId("targetTemp");
      if (input && !input.dataset.userChanged) input.value = targetTemp;
    }

    const capacity = numeric(status.POOLCAP, 0);
    if (capacity >= 100 && capacity <= 3000) {
      const input = byId("poolCapacity");
      if (input && !input.dataset.userChanged) input.value = capacity;
    }
  }

  function renderEstimate(status) {
    const estimate = numeric(status.ESTIMATE, 0);
    const buffer = numeric(status.BUFFER, 0);
    const kwh = numeric(status.ESTIMATED_KWH, 0);
    const cost = numeric(status.ESTIMATED_COST, 0);

    if (estimate >= 999) {
      setHtml("statusEstimate", '<span style="color:#ff9800">Unter den aktuellen Bedingungen nicht berechenbar</span>');
      setText("statusBuffer", "--");
      setText("statusCost", "--");
      return;
    }

    if (estimate <= 0) {
      setText("statusEstimate", "Zieltemperatur bereits erreicht");
      setText("statusBuffer", "--");
      setText("statusCost", "0,00 € (0,00 kWh)");
      return;
    }

    setText("statusEstimate", duration(estimate * 3600));
    setText("statusBuffer", buffer > 0 ? duration(buffer * 3600) : "--");
    setText(
      "statusCost",
      kwh > 0
        ? cost.toFixed(2).replace(".", ",") + " € (" + kwh.toFixed(2).replace(".", ",") + " kWh)"
        : "--"
    );
  }

  function renderRemainingHeating(status) {
    const rowVisible = Boolean(status.HEATER) || numeric(status.ESTIMATE, 0) <= 0;
    show("statusRemainingRow", rowVisible, "table-row");
    if (!rowVisible) return;

    const remaining = numeric(status.REMAINING_HEATING_TIME, -1);
    if (remaining === 0 || numeric(status.ESTIMATE, 0) <= 0) {
      setHtml("statusRemaining", '<span style="color:#4caf50">Zieltemperatur bereits erreicht</span>');
    } else if (remaining >= 999) {
      setHtml("statusRemaining", '<span style="color:#ff9800">Berechnung nicht möglich</span>');
    } else if (remaining > 0) {
      setText("statusRemaining", duration(remaining * 3600));
    } else {
      setText("statusRemaining", "--");
    }
  }

  function renderOperatingState(status) {
    if (status.HEATER) {
      setHtml("statusHeater", '<span style="color:#4caf50;font-weight:bold">🔥 EIN</span>');
      setHtml("statusReadingStateText", '<span style="color:#4caf50;font-weight:bold">🔥 Automatisches Aufheizen läuft.</span>');
      show("statusReadingState", true, "table-row");
      return;
    }

    setHtml("statusHeater", '<span style="color:#999">AUS</span>');
    const readingState = Math.trunc(numeric(status.READING_STATE, 0));
    const messages = {
      1: "⚙️ Temperaturmessung wird vorbereitet",
      2: "⚙️ Wassertemperatur wird übernommen",
    };

    if (messages[readingState]) {
      setHtml("statusReadingStateText", '<span style="color:#2196f3;font-weight:bold">' + messages[readingState] + "</span>");
      show("statusReadingState", true, "table-row");
    } else {
      show("statusReadingState", false);
    }
  }

  function renderStatus(status) {
    const active = Boolean(status && status.ACTIVE);
    show("statusActive", active);
    show("statusInactive", !active);
    updateInactiveForm(status || {});
    if (!active) return;

    setText("statusLabel", status.HEATER ? "Heizt" : "Aktiv");
    setText("statusTargetTemp", numeric(status.TARGETTEMP, "--"));
    setText("statusCurrentTemp", numeric(status.CURRENTTEMP, "--"));

    const accurateTemp = numeric(status.ACCURATETEMP, 0);
    setText("statusAccurateTemp", accurateTemp > 0 ? accurateTemp + " °C" : "Wird gemessen …");

    setText("statusTargetTime", germanDateTime(status.TARGETTIME));
    setText("statusStartTime", numeric(status.STARTTIME, 0) > 0 ? germanDateTime(status.STARTTIME) : "Wird berechnet …");
    setText("statusTimeRemaining", countdown(status.TIMEREMAINING));
    setText("statusTimeUntilStart", numeric(status.TIMEUNTILSTART, 0) > 0 ? countdown(status.TIMEUNTILSTART) : (status.HEATER ? "Aufheizen läuft" : "Jetzt"));
    setText("statusNextCheck", numeric(status.NEXTCHECK, 0) > 0 ? germanDateTime(status.NEXTCHECK) : "--");

    const keepOn = byId("activeKeepHeaterOn");
    if (keepOn && document.activeElement !== keepOn) keepOn.value = status.KEEPON ? "true" : "false";

    renderEstimate(status);
    renderRemainingHeating(status);
    renderOperatingState(status);
  }

  async function refreshStatus() {
    if (requestRunning || document.hidden) return;
    requestRunning = true;
    try {
      renderStatus(await postJson(API.status, {}));
    } catch (error) {
      console.error("Smart Schedule status could not be loaded:", error);
    } finally {
      requestRunning = false;
    }
  }

  function readScheduleForm() {
    const dateValue = byId("targetDateTime").value;
    const targetDate = new Date(dateValue);
    const targetTemp = numeric(byId("targetTemp").value, 0);
    const poolCapacity = numeric(byId("poolCapacity").value, 0);

    if (!dateValue || Number.isNaN(targetDate.getTime())) throw new Error("Bitte eine gültige Wunschzeit auswählen.");
    if (targetDate.getTime() <= Date.now() + 60000) throw new Error("Die Wunschzeit muss in der Zukunft liegen.");
    if (targetTemp < 20 || targetTemp > 40) throw new Error("Die Zieltemperatur muss zwischen 20 und 40 °C liegen.");
    if (poolCapacity < 100 || poolCapacity > 3000) throw new Error("Die Wassermenge muss zwischen 100 und 3000 Litern liegen.");

    return {
      TARGETTIME: Math.floor(targetDate.getTime() / 1000),
      TARGETTEMP: Math.round(targetTemp),
      KEEPON: byId("keepHeaterOn").value === "true",
      POOLCAP: Math.round(poolCapacity),
    };
  }

  function reportError(error) {
    const message = error && error.message ? error.message : String(error || "Unbekannter Fehler");
    window.alert(message);
  }

  async function setSchedule() {
    const button = byId("setScheduleBtn");
    if (button) button.disabled = true;
    try {
      await postJson(API.create, readScheduleForm());
      await refreshStatus();
    } catch (error) {
      reportError(error);
    } finally {
      if (button) button.disabled = false;
    }
  }

  async function updateKeepHeaterOn() {
    const select = byId("activeKeepHeaterOn");
    if (!select) return;
    select.disabled = true;
    try {
      await postJson(API.update, { KEEPON: select.value === "true" });
      await refreshStatus();
    } catch (error) {
      reportError(error);
      await refreshStatus();
    } finally {
      select.disabled = false;
    }
  }

  async function cancelSchedule() {
    const button = byId("cancelBtn");
    if (button) button.disabled = true;
    try {
      await postJson(API.cancel, {});
      await refreshStatus();
    } catch (error) {
      reportError(error);
    } finally {
      if (button) button.disabled = false;
    }
  }

  async function loadConfigFallback() {
    try {
      const config = await postJson(API.config, {});
      const capacity = numeric(config.POOLCAP, 0);
      if (capacity >= 100 && capacity <= 3000) byId("poolCapacity").value = capacity;
    } catch (error) {
      console.warn("Pool capacity could not be loaded:", error);
    }
  }

  function markEdited(event) {
    event.currentTarget.dataset.userChanged = "1";
  }

  function start() {
    initializeFormDefaults();
    ["targetTemp", "poolCapacity"].forEach(function (id) {
      const input = byId(id);
      if (input) input.addEventListener("input", markEdited);
    });

    loadConfigFallback();
    refreshStatus();
    pollHandle = window.setInterval(refreshStatus, POLL_INTERVAL_MS);
    document.addEventListener("visibilitychange", function () {
      if (!document.hidden) refreshStatus();
    });
  }

  window.setSchedule = setSchedule;
  window.updateKeepHeaterOn = updateKeepHeaterOn;
  window.cancelSchedule = cancelSchedule;
  window.loadSmartSchedule = start;

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", start, { once: true });
  } else {
    start();
  }

  window.addEventListener("beforeunload", function () {
    if (pollHandle) window.clearInterval(pollHandle);
  });
})();
