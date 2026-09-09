   
                 
                                                                           
   

let scheduleUpdateInterval = null;

   
                                   
   
function loadSmartSchedule() {
                                           
  const today = new Date();
  const year = today.getFullYear();
  const month = String(today.getMonth() + 1).padStart(2, "0");
  const day = String(today.getDate()).padStart(2, "0");
  document.getElementById("targetDateTime").value =
    year + "-" + month + "-" + day + "T19:00";

                                                               
  fetchGlobalTargetTemp();
  fetchPoolCapacity();

                                   
  updateScheduleStatus();
  scheduleUpdateInterval = setInterval(updateScheduleStatus, 5000);                          
}

   
                                                       
   
function fetchGlobalTargetTemp() {
  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/getsmartschedule/", true);
  xhr.setRequestHeader("Content-Type", "application/json");

  xhr.onreadystatechange = function () {
    if (xhr.readyState === 4 && xhr.status === 200) {
      try {
        const data = JSON.parse(xhr.responseText);
                                                                     
        if (data.GLOBALTARGET && data.GLOBALTARGET > 0) {
          document.getElementById("targetTemp").value = data.GLOBALTARGET;
        }
      } catch (e) {
        console.error("Failed to fetch global target:", e);
      }
    }
  };

  xhr.send("{}");
}


function fetchPoolCapacity() {
  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/getconfig/", true);
  xhr.setRequestHeader("Content-Type", "application/json");
  xhr.onreadystatechange = function () {
    if (xhr.readyState === 4 && xhr.status === 200) {
      try {
        const data = JSON.parse(xhr.responseText);
        if (data.POOLCAP >= 100 && data.POOLCAP <= 3000)
          document.getElementById("poolCapacity").value = data.POOLCAP;
      } catch (e) {}
    }
  };
  xhr.send("{}");
}

   
                                 
   
function updateScheduleStatus() {
  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/getsmartschedule/", true);
  xhr.setRequestHeader("Content-Type", "application/json");

  xhr.onreadystatechange = function () {
    if (xhr.readyState === 4 && xhr.status === 200) {
      try {
        const data = JSON.parse(xhr.responseText);
        displayScheduleStatus(data);
      } catch (e) {
        console.error("Failed to parse schedule data:", e);
      }
    }
  };

  xhr.send("{}");
}

   
                                
   
function displayScheduleStatus(data) {
  const isActive = data.ACTIVE || false;

                                             
  document.getElementById("statusActive").style.display = isActive
    ? "block"
    : "none";
  document.getElementById("statusInactive").style.display = isActive
    ? "none"
    : "block";

  if (isActive) {
                           
    document.getElementById("statusTargetTemp").textContent =
      data.TARGETTEMP || "--";
    document.getElementById("statusCurrentTemp").textContent =
      data.CURRENTTEMP || "--";
    document.getElementById("statusAccurateTemp").textContent =
      data.ACCURATETEMP && data.ACCURATETEMP > 0
        ? data.ACCURATETEMP + " °C"
        : "Wird gemessen...";

                                                     
    if (data.ESTIMATE >= 999) {
      document.getElementById("statusEstimate").innerHTML =
        '<span style="color: #ff9800;">Aufheizen unter diesen Bedingungen nicht berechenbar</span>';
      document.getElementById("statusBuffer").textContent = "--";
      document.getElementById("statusCost").textContent = "--";
    } else if (data.ESTIMATE > 0) {
      var estimateSeconds = Math.round(data.ESTIMATE * 3600);
      document.getElementById("statusEstimate").textContent =
        formatDuration(estimateSeconds);
                              
      var bufferSeconds = Math.round((data.BUFFER || 0) * 3600);
      document.getElementById("statusBuffer").textContent =
        bufferSeconds > 0 ? formatDuration(bufferSeconds) : "--";
      var estimatedKwh = Number(data.ESTIMATED_KWH || 0);
      var estimatedCost = Number(data.ESTIMATED_COST || 0);
      document.getElementById("statusCost").textContent =
        estimatedKwh > 0
          ? estimatedCost.toFixed(2) + " € (" + estimatedKwh.toFixed(2) + " kWh)"
          : "--";
    } else {
      document.getElementById("statusEstimate").textContent =
        "Zieltemperatur bereits erreicht";
      document.getElementById("statusBuffer").textContent = "--";
      document.getElementById("statusCost").textContent = "0,00 € (0,00 kWh)";
    }

    document.getElementById("activeKeepHeaterOn").value = data.KEEPON
      ? "true"
      : "false";

                                                                              
                                                                            
    var remainingRow = document.getElementById("statusRemainingRow");
    if (data.HEATER && data.REMAINING_HEATING_TIME !== undefined && data.REMAINING_HEATING_TIME >= 0) {
                                                                                    
      var remainingHours = data.REMAINING_HEATING_TIME;
      remainingRow.style.display = "table-row";
      if (remainingHours === 0) {
        document.getElementById("statusRemaining").innerHTML =
          '<span style="color: #4caf50;">Zieltemperatur bereits erreicht</span>';
      } else if (remainingHours >= 999) {
        document.getElementById("statusRemaining").innerHTML =
          '<span style="color: #ff9800;">Berechnung nicht möglich</span>';
      } else {
        var remainingSeconds = Math.round(remainingHours * 3600);
        document.getElementById("statusRemaining").textContent =
          formatDuration(remainingSeconds);
      }
    } else if (data.ESTIMATE >= 999) {
      remainingRow.style.display = "none";
    } else if (data.ESTIMATE <= 0) {
      remainingRow.style.display = "table-row";
      document.getElementById("statusRemaining").innerHTML =
        '<span style="color: #4caf50;">Zieltemperatur bereits erreicht</span>';
    } else {
      remainingRow.style.display = "none";
    }

                            
    const heaterStatus = data.HEATER
      ? '<span style="color: #4caf50; font-weight: bold;">🔥 EIN</span>'
      : '<span style="color: #999;">AUS</span>';
    document.getElementById("statusHeater").innerHTML = heaterStatus;

                                                                   
    if (data.HEATER) {
                                            
      document.getElementById("statusReadingStateText").innerHTML =
        '<span style="color: #4caf50; font-weight: bold;">🔥 Automatisches Aufheizen läuft.</span>';
      document.getElementById("statusReadingState").style.display = "table-row";
    } else if (data.READING_STATE > 0) {
                                              
      const readingStates = [
        "",
        "Temperaturmessung wird vorbereitet",
        "Wassertemperatur wird übernommen",
      ];
      document.getElementById("statusReadingStateText").innerHTML =
        '<span style="color: #2196f3; font-weight: bold;">⚙️ ' +
        readingStates[data.READING_STATE] +
        "</span>";
      document.getElementById("statusReadingState").style.display = "table-row";
    } else {
      document.getElementById("statusReadingState").style.display = "none";
    }

                         
    if (data.TARGETTIME) {
      const targetDate = new Date(data.TARGETTIME * 1000);
      document.getElementById("statusTargetTime").textContent =
        formatDateTime(targetDate);
    }

                        
    if (data.STARTTIME && data.STARTTIME > 0) {
      const startDate = new Date(data.STARTTIME * 1000);
      document.getElementById("statusStartTime").textContent =
        formatDateTime(startDate);
    } else {
      document.getElementById("statusStartTime").textContent =
        "Wird berechnet...";
    }

                                                          
    if (data.CHECKCOMPLETED) {
      document.getElementById("statusNextCheck").innerHTML =
        '<span style="color: #4caf50; font-weight: bold;">Planung abgeschlossen</span>';
    } else if (data.NEXTCHECK) {
      const nextCheckDate = new Date(data.NEXTCHECK * 1000);
      document.getElementById("statusNextCheck").textContent =
        formatDateTime(nextCheckDate);
    }

                                           
    if (data.TIMEREMAINING) {
      document.getElementById("statusTimeRemaining").textContent =
        formatDuration(data.TIMEREMAINING);
    }

                                             
    if (data.STARTTIME && data.STARTTIME > 0) {
                                                                       
      const timeUntilStart = data.TIMEUNTILSTART;
      if (timeUntilStart <= 0 && data.HEATER) {
                                                           
        document.getElementById("statusTimeUntilStart").innerHTML =
          '<span style="color: #4caf50; font-weight: bold;">Aufheizen läuft</span>';
      } else if (timeUntilStart <= 0) {
                                                                   
        document.getElementById("statusTimeUntilStart").innerHTML =
          '<span style="color: #ff9800;">Heizstart wird vorbereitet...</span>';
      } else {
        document.getElementById("statusTimeUntilStart").textContent =
          formatDuration(timeUntilStart);
      }
    } else {
                                                       
      document.getElementById("statusTimeUntilStart").innerHTML =
        '<span style="color: #2196f3;">Wird berechnet...</span>';
    }
  }
}

   
                                              
   
function formatDateTime(date) {
  const day = String(date.getDate()).padStart(2, "0");
  const month = String(date.getMonth() + 1).padStart(2, "0");
  const year = date.getFullYear();
  const hours = String(date.getHours()).padStart(2, "0");
  const minutes = String(date.getMinutes()).padStart(2, "0");

  return `${day}.${month}.${year} ${hours}:${minutes} Uhr`;
}

/**
 * Format duration in seconds to human readable format
 */
function formatDuration(seconds) {
  if (seconds < 0) {
    return "In der Vergangenheit";
  }

  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);

  let parts = [];
  if (days > 0) parts.push(`${days} Tag${days > 1 ? "e" : ""}`);
  if (hours > 0) parts.push(`${hours} Std.`);
  if (minutes > 0 || parts.length === 0) parts.push(`${minutes} Min.`);

  return parts.join(", ");
}

/**
 * Set a new smart schedule
 */
function setSchedule() {
  // Get form values
  const dateTimeStr = document.getElementById("targetDateTime").value;
  const targetTemp = parseInt(document.getElementById("targetTemp").value);
  const keepHeaterOn = document.getElementById("keepHeaterOn").value === "true";
  const poolCapacity = parseInt(document.getElementById("poolCapacity").value);

  // Validate inputs
  if (!dateTimeStr) {
    alert("Bitte gib Datum und Uhrzeit ein.");
    return;
  }

  if (poolCapacity < 100 || poolCapacity > 3000) { alert("Die Wassermenge muss zwischen 100 und 3000 Litern liegen."); return; }

  if (targetTemp < 20 || targetTemp > 40) {
    alert("Die Zieltemperatur muss zwischen 20°C und 40°C liegen.");
    return;
  }

  // Convert datetime-local value to Unix timestamp
  const targetDate = new Date(dateTimeStr);
  const targetTime = Math.floor(targetDate.getTime() / 1000);

  // Check if time is in the future
  const now = Math.floor(Date.now() / 1000);
  if (targetTime <= now) {
    alert("Die Zielzeit muss in der Zukunft liegen.");
    return;
  }

  // Prepare data
  const data = {
    TARGETTIME: targetTime,
    TARGETTEMP: targetTemp,
    KEEPON: keepHeaterOn,
    POOLCAP: poolCapacity,
  };

  // Send to backend
  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/setsmartschedule/", true);
  xhr.setRequestHeader("Content-Type", "application/json");

  xhr.onreadystatechange = function () {
    if (xhr.readyState === 4) {
      if (xhr.status === 200) {
        alert(
          "✅ Zeitplan aktiviert!\n\nDas System berechnet jetzt automatisch, wann deine Heizung starten muss.",
        );
        updateScheduleStatus();
      } else {
        alert(
          "❌ Fehler beim Aktivieren.\n\nMögliche Ursachen:\n- Uhrzeit nicht synchronisiert\n- Ungültige Parameter\n- Verbindungsfehler",
        );
      }
    }
  };

  xhr.send(JSON.stringify(data));
}

/**
 * Update heater behavior of the active smart schedule
 */
function updateKeepHeaterOn() {
  const keepHeaterOn =
    document.getElementById("activeKeepHeaterOn").value === "true";

  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/updatesmartschedule/", true);
  xhr.setRequestHeader("Content-Type", "application/json");

  xhr.onreadystatechange = function () {
    if (xhr.readyState === 4) {
      if (xhr.status === 200) {
        updateScheduleStatus();
      } else {
        alert("❌ Änderung konnte nicht gespeichert werden.");
        updateScheduleStatus();
      }
    }
  };

  xhr.send(JSON.stringify({ KEEPON: keepHeaterOn }));
}

/**
 * Cancel active schedule
 */
function cancelSchedule() {
  if (!confirm("Möchtest du den Zeitplan wirklich abbrechen?")) {
    return;
  }

  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/cancelsmartschedule/", true);
  xhr.setRequestHeader("Content-Type", "application/json");

  xhr.onreadystatechange = function () {
    if (xhr.readyState === 4) {
      if (xhr.status === 200) {
        alert("✅ Zeitplan erfolgreich abgebrochen.");
        updateScheduleStatus();
      } else {
        alert("❌ Fehler beim Abbrechen des Zeitplans.");
      }
    }
  };

  xhr.send("{}");
}

// Clean up interval when page unloads
window.addEventListener("beforeunload", function () {
  if (scheduleUpdateInterval) {
    clearInterval(scheduleUpdateInterval);
  }
});
