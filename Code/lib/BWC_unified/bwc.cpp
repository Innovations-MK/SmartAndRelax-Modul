#include "bwc.h"
#include "util.h"
#include "pitches.h"
#include <algorithm>

// SmartAndRelax main.cpp hook: persist a reason before intentional ESP restart.
extern void sarMarkedRestart(const char* reason);

// --- CMDQ save debounce / change-detect ---
static uint32_t g_cmdq_last_save_ms = 0;
static uint32_t g_cmdq_last_hash = 0;

// kleine, schnelle Hash-Funktion (FNV-1a 32bit)
static uint32_t fnv1a32(const uint8_t* data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}


BWC::BWC()
{
    //Initialize variables

    _dsp_brightness = 7;
    _cl_timestamp_s = time(nullptr);
    _filter_replace_timestamp_s = time(nullptr);
    _filter_clean_timestamp_s = _filter_replace_timestamp_s;
    _filter_rinse_timestamp_s = _filter_replace_timestamp_s;
    _uptime = 0;
    _pumptime = 0;
    _heatingtime = 0;
    _airtime = 0;
    _jettime = 0;
    _uptime_ms = 0;
    _pumptime_ms = 0;
    _heatingtime_ms = 0;
    _airtime_ms = 0;
    _jettime_ms = 0;
    _energy_daily_Ws = 0;
    _energy_power_W = 0;
    _energy_total_kWh = 0;
    _energy_cost = 0;
    _price = 1;
    _filter_rinse_interval = 7;
    _filter_clean_interval = 20;
    _filter_replace_interval = 60;
    _cl_interval = 14;
    _audio_enabled = true;
    _restore_states_on_start = false;
    _notification_time = 32;
    _next_notification_time = 32;
    _ambient_temp = 20;
    _virtual_temp_fix = -99;
    // Phase15-Harden1: deterministic per-boot diagnostic state.
    reboot_time_t = 0;
    reboot_time_str = "";
}

BWC::~BWC()
{
    stop();
};

void save_settings_cb(BWC* bwcInstance)
{
    bwcInstance->on_save_settings();
}
void scroll_text_cb(BWC* bwcInstance)
{
    bwcInstance->on_scroll_text();
}

void BWC::on_save_settings()
{
    if(++_ticker_count >= 3)
    {
        _save_settings_needed = true;
        _ticker_count = 0;
    }
}

void BWC::on_scroll_text()
{
    _scroll = true;
}


// --- SmartAndRelax FLT/HTR Safe-State Guard ---
static const char* SAR_SAFE_STATES_FILE = "/safe_states.txt";
static const uint32_t SAR_RECENT_COMMAND_WINDOW_MS = 15000UL;
static const uint32_t SAR_RECENT_BUTTON_WINDOW_MS  = 15000UL;
static const uint32_t SAR_RESTORE_MIN_GAP_MS       = 8000UL;
// Wenn die Filterpumpe plötzlich AUS gelesen wird, obwohl der letzte sichere
// Zustand EIN war, nicht sofort wieder einschalten. Bei einzelnen Pumpen/Displays
// wird ein echtes manuelles AUS nicht immer zuverlässig als Tastendruck erkannt.
// Bleibt AUS stabil, wird es als gewolltes AUS übernommen.
static const uint32_t SAR_PUMP_OFF_OBSERVE_MS      = 30000UL;
static const uint32_t SAR_RECENT_TARGET_WINDOW_MS  = 30000UL;
static const uint32_t SAR_TARGET_RESTORE_DELAY_MS   = 20000UL;
static const uint32_t SAR_TARGET_RESTORE_GAP_MS     = 60000UL;
// A 6-wire target jump is generated through repeated real UP/DOWN presses.
// 90 s comfortably covers the full Celsius/Fahrenheit range plus unlock/loop
// delays, while still allowing the normal target guard to recover a truly
// stalled transition afterwards.
static const uint32_t SAR_TARGET_SET_MAX_MS         = 90000UL;


void BWC::beginCloudPollingGuard(uint32_t maxActiveMs)
{
    const uint32_t now = millis();
    if(!cloudPollingGuardActive())
    {
        _cloud_poll_guard_started_ms = now;
        _cloud_poll_guard_count++;

                                                                         
                                                             
        if(cio != nullptr)
        {
            _cloud_pre_pump = cio->cio_states.pump ? 1 : 0;
            _cloud_pre_heat = cio->cio_states.heat ? 1 : 0;
            _cloud_pre_state_valid = true;
        }
        else
        {
            _cloud_pre_state_valid = false;
        }
        _cloud_post_restore_pending = false;
    }
    _cloud_poll_guard_until_ms = now + maxActiveMs;
}

void BWC::finishCloudPollingGuard(uint32_t recoveryMs)
{
    const uint32_t now = millis();
    if(_cloud_poll_guard_started_ms != 0)
    {
        _cloud_poll_guard_active_ms_last = (uint32_t)(now - _cloud_poll_guard_started_ms);
        if(_cloud_poll_guard_active_ms_last > _cloud_poll_guard_active_ms_max)
            _cloud_poll_guard_active_ms_max = _cloud_poll_guard_active_ms_last;
    }
    _cloud_poll_guard_until_ms = now + recoveryMs;
    _cloud_post_restore_pending = _cloud_pre_state_valid;
}

bool BWC::cloudPollingGuardActive() const
{
    return _cloud_poll_guard_until_ms != 0 && (int32_t)(_cloud_poll_guard_until_ms - millis()) > 0;
}


void BWC::_enforcePostCloudStateRestore()
{
    if(!_cloud_post_restore_pending || cloudPollingGuardActive()) return;

                                                                              
                                                                                
                                                                    
    _cloud_post_restore_pending = false;

    if(!_cloud_pre_state_valid || cio == nullptr)
    {
        _cloud_pre_state_valid = false;
        return;
    }

    bool queued = false;

    if(_cloud_pre_pump == 1 &&
       cio->cio_states.pump == 0 &&
       !_recentCommandedOff(false) &&
       !_recentPhysicalOffButton(false))
    {
        queued |= _queueStateRestoreCommand(SETPUMP, 1);
    }

    if(_cloud_pre_heat == 1 &&
       cio->cio_states.heat == 0 &&
       !_recentCommandedOff(true) &&
       !_recentPhysicalOffButton(true))
    {
        // Heating requires the filter pump. Restore the pump first when needed.
        if(cio->cio_states.pump == 0)
            queued |= _queueStateRestoreCommand(SETPUMP, 1);
        queued |= _queueStateRestoreCommand(SETHEATER, 1);
    }

    if(queued)
    {
        _last_live_restore_ms = millis();
        _live_safe_restore_count++;
    }

    _cloud_pre_state_valid = false;
}

bool BWC::_loadPersistentSafeStates()
{
    File file = LittleFS.open(F("/safe_states.txt"), "r");
    if(!file) return false;

    StaticJsonDocument<192> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    if(error) return false;

    uint8_t flt = doc[F("FLT")] | 0;
    uint8_t htr = doc[F("HTR")] | 0;
    uint8_t tgt = doc[F("TGT")] | 20;
    uint8_t unt = doc[F("UNT")] | 1; // alte safe_states.txt hatten UNT noch nicht -> Celsius annehmen
    uint8_t god = doc[F("GOD")] | 0;

    if(flt > 1 || htr > 1) return false;
    unt = unt ? 1 : 0;
    if(!_targetIsPlausibleForUnit(tgt, unt)) tgt = unt ? 20 : 68;

    _last_safe_pump = flt;
    _last_safe_heat = htr;
    _last_safe_target = tgt;
    _last_safe_unit = unt;
    _last_safe_god = god ? 1 : 0;
    _has_last_safe_states = true;
    return true;
}

void BWC::_savePersistentSafeStates()
{
    if(!_has_last_safe_states) return;

    File file = LittleFS.open(F("/safe_states.txt"), "w");
    if(!file) return;

    StaticJsonDocument<192> doc;
    doc[F("FLT")] = _last_safe_pump;
    doc[F("HTR")] = _last_safe_heat;
    doc[F("TGT")] = _last_safe_target;
    doc[F("UNT")] = _last_safe_unit;
    doc[F("GOD")] = _last_safe_god;

    serializeJson(doc, file);
    file.close();
}

void BWC::_updateLastKnownSafeStates()
{
    if(cloudPollingGuardActive()) return;
    if(cio == nullptr) return;

    const uint8_t pump = cio->cio_states.pump ? 1 : 0;
    const uint8_t heat = cio->cio_states.heat ? 1 : 0;
    const uint8_t target = _guardedTargetForSave();

    // Nur plausible Zieltemperaturen in der aktuell aktiven Einheit speichern.
    // Celsius: 20..40, Fahrenheit: 50..104. Dadurch blockiert der Guard
    // Fahrenheit-Zielwerte nicht mehr fälschlich als „ungültig“.
    if(!_targetIsPlausibleForUnit(target, cio->cio_states.unit)) return;

    _last_safe_pump = pump;
    _last_safe_heat = heat;
    _setLastSafeTargetFromCurrentUnit(target);
    _last_safe_god = cio->cio_states.godmode ? 1 : 0;
    _has_last_safe_states = true;
    _savePersistentSafeStates();
}

bool BWC::_recentCommandedOff(bool heater) const
{
    const uint32_t now = millis();
    if(heater)
        return (_last_heat_cmd_val == 0 && _last_heat_cmd_ms != 0 && (uint32_t)(now - _last_heat_cmd_ms) < SAR_RECENT_COMMAND_WINDOW_MS);
    return (_last_pump_cmd_val == 0 && _last_pump_cmd_ms != 0 && (uint32_t)(now - _last_pump_cmd_ms) < SAR_RECENT_COMMAND_WINDOW_MS);
}

bool BWC::_recentPhysicalOffButton(bool heater) const
{
    const uint32_t now = millis();
    if(heater)
        return (_last_heat_button_ms != 0 && (uint32_t)(now - _last_heat_button_ms) < SAR_RECENT_BUTTON_WINDOW_MS);
    return (_last_pump_button_ms != 0 && (uint32_t)(now - _last_pump_button_ms) < SAR_RECENT_BUTTON_WINDOW_MS);
}

bool BWC::_heatRestoreAllowedByTemp() const
{
    // WICHTIG:
    // cio_states.heat / HTR beschreibt hier den gewünschten Heizungs-EIN/AUS-Zustand
    // der Pumpe, NICHT den momentan tatsächlich bestromten Heizstab.
    //
    // Wenn die Zieltemperatur erreicht ist, darf die Pumpenelektronik den Heizstab
    // intern pausieren. Der HTR-Schalter selbst muss aber weiterhin EIN bleiben.
    //
    // Deshalb darf der Guard SETHEATER=1 nicht mehr anhand von
    // "aktuelle Temperatur >= Zieltemperatur" blockieren. Genau diese Sperre führte
    // dazu, dass ein ungewolltes HTR=0 nicht wiederhergestellt wurde, obwohl kein
    // echter SETHEATER=0 / Display-Aus-Befehl vorlag.
    //
    // Echte AUS-Befehle werden weiterhin über _recentCommandedOff(true) bzw.
    // _recentPhysicalOffButton(true) oberhalb in _enforceSafeLiveState() geschützt.
    return true;
}

bool BWC::_recentTargetChangeIntent() const
{
    const uint32_t now = millis();

    if(_last_target_cmd_ms != 0 && (uint32_t)(now - _last_target_cmd_ms) < SAR_RECENT_TARGET_WINDOW_MS)
        return true;

    if(_last_target_button_ms != 0 && (uint32_t)(now - _last_target_button_ms) < SAR_RECENT_TARGET_WINDOW_MS)
        return true;

    return false;
}

bool BWC::_targetIsPlausibleForUnit(uint8_t target, bool unitIsCelsius) const
{
    if(unitIsCelsius)
        return (target >= 20 && target <= 40);

                                                                            
                                                                            
                                                                          
                                                  
    return (target > 50 && target < 105);
}

uint8_t BWC::_convertTargetToUnit(uint8_t target, bool fromUnitIsCelsius, bool toUnitIsCelsius) const
{
    if(fromUnitIsCelsius == toUnitIsCelsius)
        return target;

    return toUnitIsCelsius ? (uint8_t)round(F2C((float)target))
                           : (uint8_t)round(C2F((float)target));
}

uint8_t BWC::_lastSafeTargetForCurrentUnit() const
{
    if(cio == nullptr) return _last_safe_target;
    return _convertTargetToUnit(_last_safe_target, _last_safe_unit, cio->cio_states.unit);
}

void BWC::_setLastSafeTargetFromCurrentUnit(uint8_t target)
{
    if(cio != nullptr) _last_safe_unit = cio->cio_states.unit ? 1 : 0;
    _last_safe_target = target;
}

void BWC::_beginTargetSetTransition(uint8_t startTarget, uint8_t goalTarget)
{
    const uint32_t now = millis();

    if(_target_set_in_progress)
        _target_set_abort_count++;

    _target_set_start = startTarget;
    _target_set_goal = goalTarget;
    _target_set_last_live = startTarget;
    _target_set_started_ms = now;
    _target_set_last_progress_ms = now;

                                                                              
                                                              
    _target_set_in_progress = (cio != nullptr &&
                               _targetIsPlausibleForUnit(startTarget, cio->cio_states.unit) &&
                               _targetIsPlausibleForUnit(goalTarget, cio->cio_states.unit) &&
                               startTarget != goalTarget);
}

void BWC::_cancelTargetSetTransition()
{
    if(_target_set_in_progress)
        _target_set_abort_count++;
    _target_set_in_progress = false;
}

bool BWC::_targetSetTransitionExpected(uint8_t liveTarget) const
{
    if(!_target_set_in_progress || cio == nullptr)
        return false;

    if((uint32_t)(millis() - _target_set_started_ms) >= SAR_TARGET_SET_MAX_MS)
        return false;

    if(!_targetIsPlausibleForUnit(liveTarget, cio->cio_states.unit))
        return false;

    if(_target_set_goal > _target_set_start)
        return (liveTarget >= _target_set_start && liveTarget <= _target_set_goal);

    if(_target_set_goal < _target_set_start)
        return (liveTarget <= _target_set_start && liveTarget >= _target_set_goal);

    return (liveTarget == _target_set_goal);
}

void BWC::_observeTargetSetTransition(uint8_t liveTarget)
{
    if(!_target_set_in_progress)
        return;

    const uint32_t now = millis();

    if((uint32_t)(now - _target_set_started_ms) >= SAR_TARGET_SET_MAX_MS)
    {
        _target_set_in_progress = false;
        _target_set_abort_count++;
        return;
    }

    if(!_targetSetTransitionExpected(liveTarget))
        return;

    if(liveTarget != _target_set_last_live)
    {
        _target_set_last_live = liveTarget;
        _target_set_last_progress_ms = now;
        _target_set_progress_count++;
    }

    if(liveTarget == _target_set_goal)
    {
        _target_set_in_progress = false;
        _target_set_complete_count++;
        _target_suspicious_since_ms = 0;
    }
}

bool BWC::_targetLooksSuspicious(uint8_t target) const
{
    if(cio == nullptr)
        return false;

    if(!_targetIsPlausibleForUnit(target, cio->cio_states.unit))
        return true;

    // Beim allerersten gültigen Read gibt es noch keinen Safe-State.
    // Dann darf der aktuelle Wert als Startwert übernommen werden.
    if(!_has_last_safe_states)
        return false;

    if(target == _lastSafeTargetForCurrentUnit())
        return false;

    const uint32_t now = millis();

                                                                                   
                                                                                   
                                                                                  
    if(_last_target_cmd_ms != 0 &&
       (uint32_t)(now - _last_target_cmd_ms) < SAR_RECENT_TARGET_WINDOW_MS &&
       _last_target_cmd_val >= 0 &&
       target == (uint8_t)_last_target_cmd_val)
    {
        return false;
    }

    // Direkt am Display ist der konkrete Zielwert nicht vorher bekannt. Wenn UP/DOWN
    // frisch erkannt wurde, wird die Änderung zugelassen.
    if(_last_target_button_ms != 0 &&
       (uint32_t)(now - _last_target_button_ms) < SAR_RECENT_TARGET_WINDOW_MS)
    {
        return false;
    }

                                                                                  
                                                                                  
                                                                                      
    return true;
}

uint8_t BWC::_guardedTargetForSave() const
{
                                                                            
                                                                           
                                                                          
    if(_target_set_in_progress && _has_last_safe_states)
        return _lastSafeTargetForCurrentUnit();

    if(cloudPollingGuardActive() && _has_last_safe_states)
        return _lastSafeTargetForCurrentUnit();

    if(cio == nullptr)
        return _lastSafeTargetForCurrentUnit();

    const uint8_t target = cio->cio_states.target;
    if(_targetLooksSuspicious(target) && _has_last_safe_states)
        return _lastSafeTargetForCurrentUnit();

    return target;
}

bool BWC::_shouldBlockUnsafeStateSave() const
{
    if(cloudPollingGuardActive()) return true;
    if(!_has_last_safe_states || cio == nullptr) return false;

    if(_last_safe_pump == 1 && cio->cio_states.pump == 0 && !_recentCommandedOff(false) && !_recentPhysicalOffButton(false))
        return true;

    if(_last_safe_heat == 1 && cio->cio_states.heat == 0 && !_recentCommandedOff(true) && !_recentPhysicalOffButton(true))
        return true;

    return false;
}

void BWC::_clearInternalRestoreCommands(bool pump, bool heat)
{
    _command_que.erase(std::remove_if(_command_que.begin(), _command_que.end(),
        [pump, heat](const command_que_item& item){
            const bool isInternalRestore = (item.interval == 0 && item.xtime == 0 && item.text.length() == 0);
            if(!isInternalRestore || item.val != 1) return false;
            if(pump && item.cmd == SETPUMP) return true;
            if(heat && item.cmd == SETHEATER) return true;
            return false;
        }),
        _command_que.end());
    _save_cmdq_needed = true;
}

bool BWC::_queueStateRestoreCommand(Commands cmd, uint8_t val)
{
    // Gleiche interne Restore-Kommandos nicht mehrfach stapeln.
    //
    // WICHTIG:
    // Normale Benutzer-Zeitpläne/Zyklen dürfen den Guard nicht blockieren.
    // Ein zukünftiger geplanter SETHEATER=1 oder SETPUMP=1 Eintrag mit
    // interval > 0 bzw. echter xtime ist KEIN aktiver Restore-Befehl.
    // Sonst kann der Guard bei sporadischem FLT/HTR-OFF keinen sofortigen
    // Wiederherstellungsbefehl mehr einreihen, nur weil morgen/ später schon
    // ein geplanter EIN-Zyklus existiert.
    for(const auto& item : _command_que)
    {
        const bool isInternalRestore = (item.interval == 0 && item.xtime == 0 && item.text.length() == 0);
        if(isInternalRestore && item.cmd == cmd && item.val == val)
            return false;
    }

    command_que_item item;
    item.cmd = cmd;
    item.val = val;
    item.xtime = 0;
    item.interval = 0;
    item.text = "";
    return add_command(item);
}

void BWC::_enforceSafeLiveState(const char* reason)
{
    (void)reason;
    if(cloudPollingGuardActive()) return;
    if(!_has_last_safe_states || cio == nullptr) return;

    const uint32_t now = millis();

    // 3.1.1c Guard-Fix:
    // Ein plötzlich gelesenes FLT=0 darf den sicheren Zustand NICHT automatisch
    // auf AUS umschreiben. Genau das konnte in 3.1.1 dazu führen, dass eine
    // laufende Pumpe/Heizung nach einem kurzen Kommunikationsfehler aus blieb.
    //
    // Ausnahme: Wenn nur die reine Filterpumpe lief (HTR war sicher AUS), wird
    // ein FLT=0 kurz beobachtet. Bleibt es stabil AUS, wird dieser Zustand als
    // manuelles Display-AUS akzeptiert. Das schützt die betroffenen S100101-
    // Fälle, bei denen der Display-Pumpenbutton nicht zuverlässig erkannt wird.
    //
    // War die Heizung sicher EIN, gibt es kein stilles Lernen von AUS. Dann ist
    // FLT=0/HTR=0 ohne echten AUS-Intent ein Fehlerzustand und wird restored.
    const bool pumpLooksUnexpectedOff = (_last_safe_pump == 1 &&
                                        cio->cio_states.pump == 0 &&
                                        !_recentCommandedOff(false) &&
                                        !_recentPhysicalOffButton(false));

    const bool mayObservePurePumpOff = (pumpLooksUnexpectedOff && _last_safe_heat == 0);

    if(mayObservePurePumpOff)
    {
        if(_pump_off_observe_since_ms == 0)
        {
            _pump_off_observe_since_ms = now;
            return;
        }

        if((uint32_t)(now - _pump_off_observe_since_ms) < SAR_PUMP_OFF_OBSERVE_MS)
            return;

        // Nur reine Filterpumpe war sicher EIN, Heizung war sicher AUS:
        // FLT=0 blieb stabil, daher als manuelles Pumpen-AUS akzeptieren.
        _last_safe_pump = 0;
        _last_safe_heat = 0;
        _has_last_safe_states = true;
        if(cio != nullptr)
        {
            _setLastSafeTargetFromCurrentUnit(_guardedTargetForSave());
            _last_safe_god = cio->cio_states.godmode ? 1 : 0;
        }
        _pump_off_observe_since_ms = 0;
        _last_live_restore_ms = now;
        _clearInternalRestoreCommands(true, true);
        _savePersistentSafeStates();
        return;
    }

    // Sobald die Pumpe wieder EIN ist, ein echter AUS-Intent frisch erkannt wurde
    // oder HTR sicher EIN war, ist die Beobachtung hinfällig.
    if(cio->cio_states.pump != 0 || _recentCommandedOff(false) || _recentPhysicalOffButton(false) || _last_safe_heat == 1)
        _pump_off_observe_since_ms = 0;

    if(_last_live_restore_ms != 0 && (uint32_t)(now - _last_live_restore_ms) < SAR_RESTORE_MIN_GAP_MS)
        return;

    bool queued = false;

    if(_last_safe_pump == 1 && cio->cio_states.pump == 0 && !_recentCommandedOff(false) && !_recentPhysicalOffButton(false))
    {
        queued |= _queueStateRestoreCommand(SETPUMP, 1);
    }

    // Heat-Restore nur, wenn auch die Filterpumpe als sicher EIN gespeichert ist.
    // Sonst könnte SETHEATER=1 bei Pumpen-AUS indirekt wieder die Filterpumpe starten.
    if(_last_safe_heat == 1 && _last_safe_pump == 1 && cio->cio_states.heat == 0 && !_recentCommandedOff(true) && !_recentPhysicalOffButton(true))
    {
        if(_heatRestoreAllowedByTemp())
        {
            // Heizung benötigt bei vielen Pumpen eine laufende Filterpumpe.
            if(cio->cio_states.pump == 0)
                queued |= _queueStateRestoreCommand(SETPUMP, 1);
            queued |= _queueStateRestoreCommand(SETHEATER, 1);
        }
    }

    if(queued)
    {
        _last_live_restore_ms = now;
        _live_safe_restore_count++;
    }
}




void BWC::_enforceSafeTargetState(const char* reason)
{
    (void)reason;
    if(cloudPollingGuardActive()) return;
    if(!_has_last_safe_states || cio == nullptr) return;

    const uint8_t liveTarget = cio->cio_states.target;
    const uint32_t now = millis();

    // A commanded 6-wire target change legitimately passes through many
    // intermediate values. Never let the SafeGuard interpret those values as
    // corruption and restore the previous target while the transition runs.
    if(_targetSetTransitionExpected(liveTarget))
    {
        _target_suspicious_since_ms = 0;
        _target_guard_deferred_for_set_count++;
        return;
    }

                                                                           
                                                                          
    if(!_targetLooksSuspicious(liveTarget))
    {
        _target_suspicious_since_ms = 0;
        return;
    }

    // _targetLooksSuspicious() berücksichtigt gültige SETTARGET-/Display-Änderungen
    // bereits. Wenn wir hier sind, ist der Live-Wert trotz eventueller alter Commands
    // verdächtig und darf nicht einfach wegen eines frischen, aber anderen Commands
    // ignoriert werden.

    if(liveTarget == _lastSafeTargetForCurrentUnit())
    {
        _target_suspicious_since_ms = 0;
        return;
    }

    if(_target_suspicious_since_ms == 0)
    {
        _target_suspicious_since_ms = now;
        return;
    }

    if((uint32_t)(now - _target_suspicious_since_ms) < SAR_TARGET_RESTORE_DELAY_MS)
        return;

    if(_last_target_restore_ms != 0 && (uint32_t)(now - _last_target_restore_ms) < SAR_TARGET_RESTORE_GAP_MS)
        return;

    // Erst den echten Pumpen-Zielwert aktiv anzeigen/lesen lassen, dann den
    // sicheren Wert setzen. Dadurch wird auch der Fall korrigiert, bei dem der
    // interne Web-/Diag-Wert noch 28 °C zeigt, die Pumpeneinheit selbst aber
    // nach einem Crash z. B. 38 °C gespeichert hat.
    const bool queuedRead = _queueStateRestoreCommand(GETTARGET, 0);
    const bool queuedSet  = _queueStateRestoreCommand(SETTARGET, _lastSafeTargetForCurrentUnit());
    if(queuedRead || queuedSet)
    {
        _last_target_restore_ms = now;
        _target_live_restore_count++;
        // Ab jetzt wieder warten, damit kein Restore-Sturm entsteht.
        _target_suspicious_since_ms = 0;
    }
}

void BWC::getPumpDiag(String &rtn)
{
    rtn += F("\n\n--- Pump state/command diag ---");

    rtn += F("\nstateSaveGuardSkips: ");
    rtn += String(_state_guard_skip_count);

    rtn += F("\ncloudPollGuardActive: ");
    rtn += cloudPollingGuardActive() ? F("1") : F("0");

    rtn += F("\ncloudPollGuardRemainingMs: ");
    rtn += String(cloudPollingGuardActive() ? (uint32_t)(_cloud_poll_guard_until_ms - millis()) : 0UL);

    rtn += F("\ncloudPollGuardCount: ");
    rtn += String(_cloud_poll_guard_count);

    rtn += F("\ncloudPollGuardLastActiveMs: ");
    rtn += String(_cloud_poll_guard_active_ms_last);

    rtn += F("\ncloudPollGuardMaxActiveMs: ");
    rtn += String(_cloud_poll_guard_active_ms_max);

    rtn += F("\ncloudPollGuardStateSkips: ");
    rtn += String(_cloud_poll_guard_state_skip_count);

    rtn += F("\ncloudPollGuardSaveSkips: ");
    rtn += String(_cloud_poll_guard_save_skip_count);

    rtn += F("\nliveSafeRestoreCount: ");
    rtn += String(_live_safe_restore_count);

    rtn += F("\nlastLiveSafeRestoreAgeMs: ");
    rtn += String((_last_live_restore_ms == 0) ? 0UL : (uint32_t)(millis() - _last_live_restore_ms));

    rtn += F("\npumpOffObserveAgeMs: ");
    rtn += String((_pump_off_observe_since_ms == 0) ? 0UL : (uint32_t)(millis() - _pump_off_observe_since_ms));

    rtn += F("\ntargetLiveRestoreCount: ");
    rtn += String(_target_live_restore_count);

    rtn += F("\ntargetForceReadCount: ");
    rtn += String(_target_force_read_count);

    rtn += F("\nlastTargetRestoreAgeMs: ");
    rtn += String((_last_target_restore_ms == 0) ? 0UL : (uint32_t)(millis() - _last_target_restore_ms));

    rtn += F("\ntargetSuspiciousAgeMs: ");
    rtn += String((_target_suspicious_since_ms == 0) ? 0UL : (uint32_t)(millis() - _target_suspicious_since_ms));

    rtn += F("\ntargetSetInProgress: ");
    rtn += _target_set_in_progress ? F("1") : F("0");

    rtn += F("\ntargetSetStart: ");
    rtn += String(_target_set_start);

    rtn += F("\ntargetSetGoal: ");
    rtn += String(_target_set_goal);

    rtn += F("\ntargetSetLastLive: ");
    rtn += String(_target_set_last_live);

    rtn += F("\ntargetSetAgeMs: ");
    rtn += String((_target_set_started_ms == 0) ? 0UL : (uint32_t)(millis() - _target_set_started_ms));

    rtn += F("\ntargetSetLastProgressAgeMs: ");
    rtn += String((_target_set_last_progress_ms == 0) ? 0UL : (uint32_t)(millis() - _target_set_last_progress_ms));

    rtn += F("\ntargetSetProgressCount: ");
    rtn += String(_target_set_progress_count);

    rtn += F("\ntargetSetCompleteCount: ");
    rtn += String(_target_set_complete_count);

    rtn += F("\ntargetSetAbortCount: ");
    rtn += String(_target_set_abort_count);

    rtn += F("\ntargetGuardDeferredForSetCount: ");
    rtn += String(_target_guard_deferred_for_set_count);

    rtn += F("\nhasLastSafeStates: ");
    rtn += _has_last_safe_states ? F("1") : F("0");

    rtn += F("\nlastSafePump: ");
    rtn += String(_last_safe_pump);

    rtn += F("\nlastSafeHeat: ");
    rtn += String(_last_safe_heat);

    rtn += F("\nlastSafeTarget: ");
    rtn += String(_last_safe_target);

    rtn += F("\nlastSafeUnit: ");
    rtn += String(_last_safe_unit);

    rtn += F("\nlastSafeTargetCurrentUnit: ");
    rtn += String(_lastSafeTargetForCurrentUnit());

    rtn += F("\nheatRestoreAllowedByTemp: ");
    rtn += _heatRestoreAllowedByTemp() ? F("1") : F("0");

    rtn += F("\nlastSafeGod: ");
    rtn += String(_last_safe_god);

    rtn += F("\nlastPumpCmdVal: ");
    rtn += String(_last_pump_cmd_val);

    rtn += F("\nlastHeatCmdVal: ");
    rtn += String(_last_heat_cmd_val);

    rtn += F("\nlastTargetCmdVal: ");
    rtn += String(_last_target_cmd_val);

    rtn += F("\nlastPumpCmdAgeMs: ");
    rtn += String((_last_pump_cmd_ms == 0) ? 0UL : (uint32_t)(millis() - _last_pump_cmd_ms));

    rtn += F("\nlastHeatCmdAgeMs: ");
    rtn += String((_last_heat_cmd_ms == 0) ? 0UL : (uint32_t)(millis() - _last_heat_cmd_ms));

    rtn += F("\nlastTargetCmdAgeMs: ");
    rtn += String((_last_target_cmd_ms == 0) ? 0UL : (uint32_t)(millis() - _last_target_cmd_ms));

    rtn += F("\nlastPumpButtonAgeMs: ");
    rtn += String((_last_pump_button_ms == 0) ? 0UL : (uint32_t)(millis() - _last_pump_button_ms));

    rtn += F("\nlastHeatButtonAgeMs: ");
    rtn += String((_last_heat_button_ms == 0) ? 0UL : (uint32_t)(millis() - _last_heat_button_ms));

    rtn += F("\nlastTargetButtonAgeMs: ");
    rtn += String((_last_target_button_ms == 0) ? 0UL : (uint32_t)(millis() - _last_target_button_ms));

    if(cio != nullptr)
    {
        rtn += F("\ncioModel: ");
        rtn += cio->getModel();

        rtn += F("\nlivePowerState: ");
        rtn += String(cio->cio_states.power ? 1 : 0);

        rtn += F("\nliveLockedState: ");
        rtn += String(cio->cio_states.locked ? 1 : 0);

        rtn += F("\nlivePumpState: ");
        rtn += String(cio->cio_states.pump ? 1 : 0);

        rtn += F("\nliveHeatState: ");
        rtn += String(cio->cio_states.heat ? 1 : 0);

        rtn += F("\nliveTargetState: ");
        rtn += String(cio->cio_states.target);

        rtn += F("\nliveTemperatureState: ");
        rtn += String(cio->cio_states.temperature);

        rtn += F("\nliveUnitCelsius: ");
        rtn += String(cio->cio_states.unit ? 1 : 0);

        rtn += F("\nactualPumpTargetForDisplay: ");
        rtn += String(cio->cio_states.target);

        rtn += F("\nguardedTargetForDisplay: ");
        rtn += String(_guardedTargetForSave());

        rtn += F("\nforceNextSetTarget: ");
        rtn += _force_next_settarget ? F("1") : F("0");

        rtn += F("\nliveBrightnessState: ");
        rtn += String(cio->cio_states.brightness);

        rtn += F("\nliveGodState: ");
        rtn += String(cio->cio_states.godmode ? 1 : 0);

        rtn += F("\nliveDisplayChars: ");
        rtn += (char)cio->cio_states.char1;
        rtn += (char)cio->cio_states.char2;
        rtn += (char)cio->cio_states.char3;

        rtn += F("\ntargetCaptureAgeMs: ");
        if(cio->target_capture_last_button_ms == 0) rtn += F("none");
        else rtn += String((uint32_t)(millis() - cio->target_capture_last_button_ms));

        rtn += F("\ntargetCaptureArmCount: ");
        rtn += String(cio->target_capture_arm_count);

        rtn += F("\ntargetCaptureAcceptCount: ");
        rtn += String(cio->target_capture_accept_count);

        rtn += F("\ntargetCaptureLastValue: ");
        rtn += String(cio->target_capture_last_value);

        rtn += F("\ntargetCaptureLastValueAgeMs: ");
        if(cio->target_capture_last_value_ms == 0) rtn += F("none");
        else rtn += String((uint32_t)(millis() - cio->target_capture_last_value_ms));

        rtn += F("\nliveButtonQueueLength: ");
        rtn += String(cio->_button_que_len);

        rtn += F("\nphysicalTargetPreemptCount: ");
        rtn += String(cio->physical_target_preempt_count);

        rtn += F("\nphysicalTargetImmediateStartCount: ");
        rtn += String(cio->physical_target_immediate_start_count);

        rtn += F("\ncioGoodPackets: ");
        rtn += String(cio->good_packets_count);

        rtn += F("\ncioBadPackets: ");
        rtn += String(cio->bad_packets_count);

        rtn += F("\ntargetLooksSuspicious: ");
        rtn += _targetLooksSuspicious(cio->cio_states.target) ? F("1") : F("0");

        rtn += F("\nguardedTargetForSave: ");
        rtn += String(_guardedTargetForSave());

        rtn += F("\ntargetChangeIntent: ");
        rtn += _recentTargetChangeIntent() ? F("1") : F("0");
    }
    else
    {
        rtn += F("\ncioModel: (none)");
        rtn += F("\nlivePowerState: -1");
        rtn += F("\nliveLockedState: -1");
        rtn += F("\nlivePumpState: -1");
        rtn += F("\nliveHeatState: -1");
        rtn += F("\nliveTargetState: -1");
        rtn += F("\nliveTemperatureState: -1");
        rtn += F("\nliveUnitCelsius: -1");
        rtn += F("\nliveBrightnessState: -1");
        rtn += F("\nliveGodState: -1");
        rtn += F("\nliveDisplayChars: ---");
        rtn += F("\ntargetCaptureAgeMs: -1");
        rtn += F("\ntargetCaptureArmCount: -1");
        rtn += F("\ntargetCaptureAcceptCount: -1");
        rtn += F("\ntargetCaptureLastValue: -1");
        rtn += F("\ntargetCaptureLastValueAgeMs: -1");
        rtn += F("\nliveButtonQueueLength: -1");
        rtn += F("\nphysicalTargetPreemptCount: -1");
        rtn += F("\nphysicalTargetImmediateStartCount: -1");
        rtn += F("\ncioGoodPackets: -1");
        rtn += F("\ncioBadPackets: -1");
        rtn += F("\ntargetLooksSuspicious: -1");
        rtn += F("\nguardedTargetForSave: -1");
        rtn += F("\ntargetChangeIntent: -1");
    }

    if(dsp != nullptr)
    {
        rtn += F("\ndspGoodPackets: ");
        rtn += String(dsp->good_packets_count);
        rtn += F("\ndspBadPackets: ");
        rtn += String(dsp->bad_packets_count);
    }
    else
    {
        rtn += F("\ndspGoodPackets: -1");
        rtn += F("\ndspBadPackets: -1");
    }
}


void BWC::setup(void){
    if(cio != nullptr) delete cio;
    if(dsp != nullptr) delete dsp;
    Models ciomodel;
    Models dspmodel;
    std::optional<Power> power_levels = {};
    
    if(!_loadHardware(ciomodel, dspmodel, pins, power_levels)){
        pins[0] = D1;
        pins[1] = D2;
        pins[2] = D3;
        pins[3] = D4;
        pins[4] = D5;
        pins[5] = D6;
        pins[6] = D7;
        pins[7] = D8;
        
    }
    // Serial.printf("Cio loaded: %d, dsp model: %d\n", ciomodel, dspmodel);
    for(int i = 0; i < 8; i++)
    {
        // Serial.printf("pin%d: %d\n", i, pins[i]);
    }
    {
        HeapSelectIram ephemeral;
        switch(ciomodel)
        {
            case PRE2021:
                cio = new CIO_PRE2021;
                break;
            case MIAMI2021:
                cio = new CIO_2021;
                break;
            case MALDIVES2021:
                cio = new CIO_2021_HJT;
                break;
            case M54149E:
                cio = new CIO_54149E;
                break;
            case M54173:
                cio = new CIO_54173;
                break;
            case M54154:
                cio = new CIO_54154;
                break;
            case M54144:
                cio = new CIO_54144;
                break;
            case M54138:
                cio = new CIO_54138;
                break;
            case M54123:
                cio = new CIO_54123;
                break;
            default:
                cio = new CIO_PRE2021;
                break;
        }
        switch(dspmodel)
        {
            case PRE2021:
                dsp = new DSP_PRE2021;
                break;
            case MIAMI2021:
                dsp = new DSP_2021;
                break;
            case MALDIVES2021:
                dsp = new DSP_2021_HJT;
                break;
            case M54149E:
                dsp = new DSP_54149E;
                break;
            case M54173:
                dsp = new DSP_54173;
                break;
            case M54154:
                dsp = new DSP_54154;
                break;
            case M54144:
                dsp = new DSP_54144;
                break;
            case M54138:
                dsp = new DSP_54138;
                break;
            case M54123:
                dsp = new DSP_54123;
                break;
            default:
                dsp = new DSP_PRE2021;
                break;
        }
    }
    cio->setup(pins[0], pins[1], pins[2]);

    cio->setPowerLevels(power_levels);
    
    dsp->setup(pins[3], pins[4], pins[5], pins[6]);
    tempSensorPin = pins[7];
    hasjets = cio->getHasjets();
    hasgod = cio->getHasgod();
    cio->cio_toggles.power_change = 1;
    begin();
}

void BWC::begin(){
    // _save_melody("melody.bin");
    // if(_audio_enabled) dsp->playIntro();
    // dsp->LEDshow();
    _save_settings_ticker.attach(3600.0f, save_settings_cb, this);
    _scroll_text_ticker.attach(0.25f, scroll_text_cb, this);

    _next_notification_time = _notification_time;
    loadCommandQueue();
    _loadSettings();
    _loadPersistentSafeStates();
    _restoreStates();
    _loadSmartSchedule();
}


void BWC::loop(){
    ++loop_count;
    // if(loop_count % 100 == 0) Serial.printf_P(PSTR("bwc loop %d\n"), millis());
    #ifdef ESP8266
    ESP.wdtFeed();
    #endif
    _timestamp_secs = time(nullptr);
    _updateTimes();
    if(_scroll && (dsp->text.length() > 0)) 
    {
        dsp->text.remove(0,1);
        _scroll = false;
    }
    cio->updateStates();                //checking serial line
    dsp->dsp_states = cio->cio_states;  //
    
    /*Modify and use dsp->dsp_states here if we want to show text or something*/
    dsp->setRawPayload(cio->getRawPayload());
    dsp->setSerialReceived(cio->getSerialReceived());
    /*Increase screen brightness when pressing buttons*/
    adjust_brightness();

    dsp->handleStates();                //transmits to dsp if serial received from cio
    dsp->updateToggles();               //checking serial line

    // Display-Tastendruecke sofort erfassen, bevor Smart Schedule oder der
    // Safe-State-Guard darauf reagieren. So wird ein manueller AUS-Wunsch
    // nicht durch einen unmittelbar erneut eingereihten Heiz-EIN-Befehl
    // ueberstimmt.
    if(dsp->dsp_toggles.pressed_button == PUMP) _last_pump_button_ms = millis();
    if(dsp->dsp_toggles.pressed_button == HEAT) _last_heat_button_ms = millis();

    _handleSmartSchedulePanelOverride();

    cio->cio_toggles = dsp->dsp_toggles;

    play_sound();

    if(_dsp_tgt_used)
        cio->cio_toggles.target = cio->cio_states.target;
    else
        cio->cio_toggles.target = _web_target;
        
    if(dsp->dsp_toggles.unit_change)
    {
        cio->cio_states.unit ? cio->cio_toggles.target = C2F(cio->cio_toggles.target) : cio->cio_toggles.target = F2C(cio->cio_toggles.target); 
    }
    
    /*following method will change target temp and set _dsp_tgt_used to false if target temp is changed*/
    _handleCommandQ();
    _handleSmartSchedule();

    /*If new target was not set above, use whatever the cio says*/
    cio->setRawPayload(dsp->getRawPayload());
    cio->setSerialReceived(dsp->getSerialReceived());
    cio->handleToggles();               //transmits to cio if serial received from dsp

    if(_save_settings_needed) saveSettings();
    if(_save_cmdq_needed) _saveCommandQueue();
    if(_save_states_needed) _saveStates();
    if(_save_smartschedule_needed) _saveSmartSchedule();
    _handleNotification();
    _handleStateChanges();
    _enforcePostCloudStateRestore();
    _calcVirtualTemp();
    // logstates();
    if(BWC_DEBUG) _log();
}

void BWC::_log()
{
    static uint32_t writes = 0;
    static std::vector<uint8_t> prev_fromcio;
    static std::vector<uint8_t> prev_fromdsp;
    static std::vector<uint8_t> prev_tocio;
    static std::vector<uint8_t> prev_todsp;
    std::vector<uint8_t> fromcio = cio->getRawPayload();
    std::vector<uint8_t> fromdsp = dsp->getRawPayload();
    std::vector<uint8_t> tocio = cio->_raw_payload_to_cio;
    std::vector<uint8_t> todsp = dsp->_raw_payload_to_dsp;

    if((fromcio == prev_fromcio) && (fromdsp == prev_fromdsp) && (tocio == prev_tocio) && (todsp == prev_todsp)) return;
    prev_fromcio = fromcio;
    prev_fromdsp = fromdsp;
    prev_tocio = tocio;
    prev_todsp = todsp;
    
    File file = LittleFS.open(F("log.txt"), "a");
    if (!file) {
        // Serial.println(F("Failed to save states.txt"));
        return;
    }
    if(++writes > 1000) 
    {
        file.printf_P(PSTR("\n**** MAX LENGTH OF FILE REACHED. DELETE FILE TO LOG AGAIN"));
        return;
    }

    tm * p_time_tm = gmtime((time_t*) &_timestamp_secs);
    char tm_string[64];
    strftime(tm_string, 64, "%F %T", p_time_tm);
    file.print(tm_string);
    file.printf_P(PSTR("UTC.  SW:%s \nCIO-ESP:"), FW_VERSION);
    for(unsigned int i = 0; i< fromcio.size(); i++)
    {
        if(i>0)file.print(',');
        file.print(fromcio[i], HEX);
    }
    file.print(F("\nDSP-ESP:"));
    for(unsigned int i = 0; i< fromdsp.size(); i++)
    {
        if(i>0)file.print(',');
        file.print(fromdsp[i], HEX);
    }
    file.print(F("\nESP-CIO:"));
    for(unsigned int i = 0; i< tocio.size(); i++)
    {
        if(i>0)file.print(',');
        file.print(tocio[i], HEX);
    }
    file.print(F("\nESP-DSP:"));
    for(unsigned int i = 0; i< todsp.size(); i++)
    {
        if(i>0)file.print(',');
        file.print(todsp[i], HEX);
    }
    file.printf_P(PSTR("\nCIO msg count: %d DSP msg count: %d"), cio->good_packets_count, dsp->good_packets_count);
    float CIO_quality, DSP_quality;
    if(cio->good_packets_count == 0 && cio->bad_packets_count == 0)
      CIO_quality = 0;
    else
      CIO_quality = 100 * cio->good_packets_count / (cio->good_packets_count + cio->bad_packets_count);
    if(dsp->good_packets_count == 0 && dsp->bad_packets_count == 0)
      DSP_quality = 0;
    else
      DSP_quality = 100 * dsp->good_packets_count / (dsp->good_packets_count + dsp->bad_packets_count);
    file.printf_P(PSTR("\nCIO msg quality: %f%% DSP msg quality: %f%% (Only useful in 4 wire pumps)\n\n"), CIO_quality, DSP_quality);
    file.close();
}

void BWC::adjust_brightness()
{
    if(dsp->dsp_toggles.pressed_button != NOBTN) _override_dsp_brt_timer = 5000;
    if(_override_dsp_brt_timer > 0)
    {
        dsp->dsp_states.brightness = _dsp_brightness + 1;
        if(dsp->dsp_states.brightness > 8) dsp->dsp_states.brightness = 8;
    }
    else
    {
        dsp->dsp_states.brightness = _dsp_brightness;
    }
}

void BWC::play_sound()
{
    if(!dsp->dsp_states.locked && dsp->dsp_states.power)
    {
        switch(dsp->dsp_toggles.pressed_button)
        {
            case UP:
                if(dsp->EnabledButtons[UP]) _beep();
                _cancelTargetSetTransition();
                _dsp_tgt_used = true;
                _last_target_button_ms = millis();
                break;
            case DOWN:
                if(dsp->EnabledButtons[DOWN]) _beep();
                _cancelTargetSetTransition();
                _dsp_tgt_used = true;
                _last_target_button_ms = millis();
                break;
            case TIMER:
                if(dsp->EnabledButtons[TIMER]) _beep();
                break;
            default:

                break;
        }
    }

    if
    (
        dsp->dsp_toggles.bubbles_change || dsp->dsp_toggles.heat_change || 
        dsp->dsp_toggles.jets_change    || dsp->dsp_toggles.power_change || 
        dsp->dsp_toggles.pump_change    || dsp->dsp_toggles.unit_change
    ) 
        _accord();
    /* Lock button sound is taken care of in _handleStateChanges() */
}

void BWC::stop(){
    _save_settings_ticker.detach();
    _scroll_text_ticker.detach();
    if(cio != nullptr){
        Serial.println(F("stopping cio"));
        cio->stop();
        Serial.println(F("del cio"));
        delete cio;
        cio = nullptr;
    }
    if(dsp != nullptr)
    {
        Serial.println(F("stopping dsp"));
        dsp->stop();
        Serial.println(F("del dsp"));
        delete dsp;
        dsp = nullptr;
    }
}

void BWC::pause_all(bool action)
{
    if(action)
    {
        if(_save_settings_ticker.active()) _save_settings_ticker.detach();
        if(_scroll_text_ticker.active()) _scroll_text_ticker.detach();
    } else
    {
        _save_settings_ticker.attach(3600.0f, save_settings_cb, this);
        _scroll_text_ticker.attach(0.25f, scroll_text_cb, this);
    }
    if(cio != nullptr)
        cio->pause_all(action);
    if(dsp != nullptr)
        dsp->pause_all(action);
}

/*Sort by xtime, ascending*/
bool BWC::_compare_command(const command_que_item& i1, const command_que_item& i2)
{
    return i1.xtime < i2.xtime;
}

void BWC::_handleNotification()
{
    /* user don't want a notification*/
    if(!_notify) return;
    /* there is no upcoming command*/
    if(_command_que.size() == 0)
    {
        _next_notification_time = _notification_time;
        return;
    }
    /* not the time yet*/
    if((int64_t)_command_que[0].xtime - (int64_t)_timestamp_secs > (int64_t)_next_notification_time) return;
    /* only _notify for these commands*/
    if(!(_command_que[0].cmd == SETBUBBLES || _command_que[0].cmd == SETHEATER || _command_que[0].cmd == SETJETS || _command_que[0].cmd == SETPUMP)) return;

    if(_audio_enabled) _sweepup();
    dsp->text += "  --" + String(_next_notification_time) + "--";
    // dsp->dsp_states.text = "i-i-";
    if(_next_notification_time <= 2)
        _next_notification_time = -10; //postpone "alarm" until after the command xtime (will be reset on command execution)
    else
        _next_notification_time /= 2;
}

void BWC::_handleCommandQ() {
    if(_command_que.size() < 1) return;
    /* time for next command? */
    if (_timestamp_secs < _command_que[0].xtime) return;
    //If interval > 0 then append to commandQ with updated xtime.
    if(_command_que[0].interval > 0)
    {
        while(_command_que[0].xtime < (uint64_t)time(nullptr))
            _command_que[0].xtime += _command_que[0].interval;
       _command_que.push_back(_command_que[0]);
    } 
    _handlecommand(_command_que[0].cmd, _command_que[0].val, _command_que[0].text);
}

bool BWC::_handlecommand(Commands cmd, int64_t val, const String& txt="")
{
    bool restartESP = false;
    
                                                              
                                                                               
                                                                               
    if(txt.length() > 0) dsp->text += String(" ") + txt;
    switch (cmd)
    {
    case SETTARGET:
    {
        if(! ((val > 0 && val < 41) || (val > 50 && val < 105)) ) break;
        bool implied_unit_is_celsius = (val < 41);
        bool required_unit = cio->cio_states.unit;
        if(implied_unit_is_celsius && !required_unit)
            cio->cio_toggles.target = round(C2F(val));
        else if(!implied_unit_is_celsius && required_unit)
            cio->cio_toggles.target = round(F2C(val));
        else
            cio->cio_toggles.target = val;

        const uint8_t requestedTarget = cio->cio_toggles.target;
        const uint8_t liveTargetAtCommand = cio->cio_states.target;

        // The explicit SETTARGET value is user intent and therefore becomes the
        // authoritative safe target immediately. This is critical for large
        // 6-wire jumps: if the ESP resets halfway through 22->40, restore 40,
        // never the previous 22. Intermediate live values remain progress only.
        _beginTargetSetTransition(liveTargetAtCommand, requestedTarget);
        _setLastSafeTargetFromCurrentUnit(requestedTarget);
        _has_last_safe_states = true;
        _target_suspicious_since_ms = 0;
        _savePersistentSafeStates();

        // Interner Guard-/Restore-Fall:
        // Wenn die Pumpeneinheit nach einem Crash intern z. B. 38 °C hält,
        // das Modul aber noch 28 °C gecacht hat, würde SETTARGET 28 sonst
        // keine Taste senden. Deshalb erzwingen wir einmalig eine echte
        // Zielwert-Übertragung, ohne TGT in JSON/WebUI dauerhaft zu maskieren.
        if(_force_next_settarget && cio->cio_toggles.target == cio->cio_states.target)
        {
            if(cio->cio_states.unit)
                cio->cio_states.target = (cio->cio_toggles.target > 20) ? (cio->cio_toggles.target - 1) : (cio->cio_toggles.target + 1);
            else
                cio->cio_states.target = (cio->cio_toggles.target > 68) ? (cio->cio_toggles.target - 1) : (cio->cio_toggles.target + 1);
        }
        _force_next_settarget = false;

        /*Send this value to cio instead of results from button presses on the display*/
        _dsp_tgt_used = false;
        _web_target = cio->cio_toggles.target;
        _last_target_cmd_ms = millis();
        _last_target_cmd_val = (int8_t)cio->cio_toggles.target;
        break;
    }
    case SETUNIT:
        // Unit changes invalidate the numeric start/goal range of an active
        // target transition. Cancel it before converting the target values.
        _cancelTargetSetTransition();
        if(hasgod && !cio->cio_toggles.godmode) break;
        if(val == 1 && cio->cio_states.unit == 0) cio->cio_toggles.target = round(F2C(cio->cio_toggles.target)); 
        if(val == 0 && cio->cio_states.unit == 1) cio->cio_toggles.target = round(C2F(cio->cio_toggles.target)); 
        if((uint8_t)val != cio->cio_states.unit) cio->cio_toggles.unit_change = 1;
        _dsp_tgt_used = false;
        _web_target = cio->cio_toggles.target;
        _last_target_cmd_ms = millis();
        _last_target_cmd_val = (int8_t)cio->cio_toggles.target;
        break;
    case SETBUBBLES:
        if(val != cio->cio_states.bubbles) cio->cio_toggles.bubbles_change = 1;
        break;
    case SETHEATER:
    {
        const uint32_t now = millis();
        _last_heat_cmd_ms = now;
        _last_heat_cmd_val = (int8_t)val;

                                                                                  
                                                                                   
                                                                          
        if(val == 1)
        {
            // Heizung EIN bedeutet bei der Lay-Z-Spa-Logik immer auch
            // Filterpumpe EIN. Deshalb muss der Safe-State beide Zustände
            // als gewollt EIN speichern, auch wenn cio_states.pump zum
            // Zeitpunkt des Befehls noch nicht aktualisiert wurde.
            _last_safe_pump = 1;
            _last_safe_heat = 1;
            _has_last_safe_states = true;
            if(cio != nullptr)
            {
                _setLastSafeTargetFromCurrentUnit(_guardedTargetForSave());
                _last_safe_god = cio->cio_states.godmode ? 1 : 0;
            }
            _savePersistentSafeStates();
        }

        // Gewolltes HEATER AUS darf vom FLT/HTR-Guard nicht wieder hergestellt werden.
        // Deshalb den sicheren Heat-State sofort auf AUS übernehmen und eventuell
        // bereits wartende Restore-Kommandos entfernen.
        if(val == 0)
        {
            _last_safe_heat = 0;
            _has_last_safe_states = true;
            if(cio != nullptr)
            {
                _last_safe_pump = cio->cio_states.pump ? 1 : 0;
                _setLastSafeTargetFromCurrentUnit(_guardedTargetForSave());
                _last_safe_god = cio->cio_states.godmode ? 1 : 0;
            }
            _last_live_restore_ms = now;
            _savePersistentSafeStates();
            // Nur interne Guard-Restore-Kommandos entfernen.
            // WICHTIG: Geplante Benutzer-Zyklen mit interval > 0 bzw. echter xtime
            // dürfen nicht gelöscht werden, sonst läuft z.B. Heizung/Filterpumpe
            // nach dem ersten AUS-Zyklus nicht mehr erneut an.
_clearInternalRestoreCommands(false, true);
        }

        if(cio != nullptr && val != cio->cio_states.heat) cio->cio_toggles.heat_change = 1;
        break;
    }
    case SETPUMP:
    {
        const uint32_t now = millis();
        _last_pump_cmd_ms = now;
        _last_pump_cmd_val = (int8_t)val;

                                                                      
                                                                                 
                                       
        if(val == 1)
        {
            _pump_off_observe_since_ms = 0;
            _last_safe_pump = 1;
            _has_last_safe_states = true;
            if(cio != nullptr)
            {
                _setLastSafeTargetFromCurrentUnit(_guardedTargetForSave());
                _last_safe_god = cio->cio_states.godmode ? 1 : 0;
            }
            _savePersistentSafeStates();
        }

        // Gewolltes PUMP AUS bedeutet bei der Lay-Z-Spa-Logik automatisch auch
        // HEATER AUS, weil ohne Filterpumpe kein Heizbetrieb möglich ist.
        // Dieser Zustand muss als gültig gespeichert werden, sonst schaltet der
        // Guard Pumpe/Heizung fälschlich direkt wieder ein.
        if(val == 0)
        {
            _pump_off_observe_since_ms = 0;
            _last_heat_cmd_ms = now;
            _last_heat_cmd_val = 0;
            _last_safe_pump = 0;
            _last_safe_heat = 0;
            _has_last_safe_states = true;
            if(cio != nullptr)
            {
                _setLastSafeTargetFromCurrentUnit(_guardedTargetForSave());
                _last_safe_god = cio->cio_states.godmode ? 1 : 0;
            }
            _last_live_restore_ms = now;
            _savePersistentSafeStates();
            // Nur interne Guard-Restore-Kommandos entfernen.
            // WICHTIG: Geplante Benutzer-Zyklen mit interval > 0 bzw. echter xtime
            // dürfen nicht gelöscht werden, sonst läuft z.B. Heizung/Filterpumpe
            // nach dem ersten AUS-Zyklus nicht mehr erneut an.
_clearInternalRestoreCommands(true, true);
        }

        if(cio != nullptr && val != cio->cio_states.pump) cio->cio_toggles.pump_change = 1;
        break;
    }
    case RESETQ:
        _command_que.clear();
        _save_cmdq_needed = true;
        _next_notification_time = _notification_time; //reset alarm time
        return false;
        break;
    case REBOOTESP:
        restartESP = true;
        break;
    case GETTARGET:
        // 3.0.0-Verhalten für Anzeige/WebUI beibehalten:
        // Die Zieltemperatur-Anzeige darf NICHT durch einen künstlichen Wert
        // wie 0 verfälscht werden. GETTARGET dient hier nur als interner Marker:
        // Der direkt folgende SETTARGET darf auch dann erzwungen werden, wenn
        // der gecachte Modulwert bereits dem Safe-Wert entspricht.
        _force_next_settarget = true;
        _target_force_read_count++;
        break;
    case RESETTIMES:
        _uptime = 0;
        _pumptime = 0;
        _jettime = 0;
        _heatingtime = 0;
        _airtime = 0;
        _uptime_ms = 0;
        _pumptime_ms = 0;
        _jettime_ms = 0;
        _heatingtime_ms = 0;
        _airtime_ms = 0;
        _energy_total_kWh = 0;
        _energy_cost = 0;
        _save_settings_needed = true;
        _new_data_available = true;
        break;
    case RESETCLTIMER:
        _cl_timestamp_s = _timestamp_secs;
        _save_settings_needed = true;
        _new_data_available = true;
        break;
    case RESETFREPLACETIMER:
        _filter_replace_timestamp_s = _timestamp_secs;
        _save_settings_needed = true;
        _new_data_available = true;
        break;
    case RESETFCLEANTIMER:
        _filter_clean_timestamp_s = _timestamp_secs;
        _save_settings_needed = true;
        _new_data_available = true;
        break;
    case RESETFRINSETIMER:
        _filter_rinse_timestamp_s = _timestamp_secs;
        _save_settings_needed = true;
        _new_data_available = true;
        break;
    case SETJETS:
        if(val != cio->cio_states.jets) cio->cio_toggles.jets_change = 1;
        break;
    case SETBRIGHTNESS:
        _dsp_brightness = val;
        _new_data_available = true;
        break;
    case SETBEEP:
        if(val == 0) _beep();
        else if(val == 1) _accord();
        else _load_melody_json(txt);
        break;
    case SETAMBIENTF:
        setAmbientTemperature(val, false);
        _new_data_available = true;
        break;
    case SETAMBIENTC:
        setAmbientTemperature(val, true);
        _new_data_available = true;
        break;
    case RESETDAILY:
        _energy_daily_Ws = 0;
        _save_settings_needed = true;
        _new_data_available = true;
        break;
    case SETGODMODE:
        cio->cio_toggles.godmode = val > 0;
        break;
    case SETFULLPOWER:
        val = std::clamp((int)val, 0, 1);
        cio->cio_toggles.no_of_heater_elements_on = val+1;
        break;
    /*PRINTTEXT is not a command per se. Every command prints the txt string, and if we ONLY want to print txt we do nothing to the command.*/
    case SETREADY:
        {
            command_que_item item;
            if((int64_t)_timestamp_secs > (int64_t)(val - _estHeatingTime() * 3600.0f - 7200)) //2 hours extra margin
            {
                /*time to start heating*/
                item.cmd = SETHEATER;
                item.interval = 0;
                item.text = "";
                item.val = 1;
                item.xtime = _timestamp_secs + 1;
                add_command(item);
            }
            else
            {
                /*Not time yet, so add check in one minute*/
                item.cmd = cmd;
                item.interval = 0;
                item.text = "";
                item.val = val;
                item.xtime = _timestamp_secs + 60;
                /*We can't use addcommand() because it will copy xtime to val again*/
                _command_que.push_back(item);
                std::sort(_command_que.begin(), _command_que.end(), _compare_command);
            }
        }
        break;
    case SETR:
        _R_COOLING = val/1000000.0f;
        _vt_calibrated = true;
        _save_settings_needed = true;
        break;
    case SETPOWER:
                                                                          
                                                                              
                                                                              
                                                                           
        if(cio != nullptr && dsp != nullptr && dsp->EnabledButtons[POWER])
        {
            val = std::clamp((int)val, 0, 1);
            if((uint8_t)val != cio->cio_states.power)
                cio->cio_toggles.power_change = 1;
        }
        break;
    case SETLOCK:
                                                                         
                                                                             
                                                                            
                                                                              
        if(cio != nullptr && dsp != nullptr && dsp->EnabledButtons[LOCK] && cio->cio_states.power)
        {
            val = std::clamp((int)val, 0, 1);
            if((uint8_t)val != cio->cio_states.locked)
                cio->cio_toggles.lock_change = 1;
        }
        break;
    default:
        break;
    }
    //remove from commandQ
    _command_que.erase(_command_que.begin());
    _next_notification_time = _notification_time; //reset alarm time
    _save_cmdq_needed = true;
    if(restartESP) {
        saveSettings();
        _saveCommandQueue();
        stop();
        delay(3000);
        sarMarkedRestart("BWC command REBOOTESP");
    }
    /*If we pushed back an item, we need to re-sort the que*/
    std::sort(_command_que.begin(), _command_que.end(), _compare_command);
    return false;
}

void BWC::_handleStateChanges()
{
    if(_prev_cio_states != cio->cio_states || _prev_dsp_states.brightness != dsp->dsp_states.brightness) _new_data_available = true;

                                                                                
                                                                           
                                                                                
                                                                       
                                                                       
    if(cloudPollingGuardActive())
    {
        _cloud_poll_guard_state_skip_count++;
    }
    if(cio->cio_states.temperature != _prev_cio_states.temperature)
    {
        _deltatemp = cio->cio_states.temperature - _prev_cio_states.temperature;
        _updateVirtualTempFix_ontempchange();
        _temp_change_timestamp_ms = millis();
    }

    // Store virtual temp data point
    if(cio->cio_states.heatred != _prev_cio_states.heatred)
    {
        _heatred_change_timestamp_ms = millis();
        _updateVirtualTempFix_onheaterchange();
    }

    if(cio->cio_states.pump != _prev_cio_states.pump)
    {
        _pump_change_timestamp_ms = millis();
    }

    if(dsp->dsp_toggles.pressed_button == PUMP) _last_pump_button_ms = millis();
    if(dsp->dsp_toggles.pressed_button == HEAT) _last_heat_button_ms = millis();

    // Wenn live plötzlich AUS gelesen wird, obwohl der letzte sichere Zustand EIN war,
    // aktiv wiederherstellen. Das ist absichtlich nur für FLT/HTR und nicht für TGT/BRT.
    _enforceSafeLiveState("state change");
    _enforceSafeTargetState("state change");

    if(cio->cio_states.bubbles != _prev_cio_states.bubbles)
    {
        _bubbles_change_timestamp_ms = millis();
    }

    if((cio->cio_states.locked != _prev_cio_states.locked) && dsp->EnabledButtons[LOCK] && _audio_enabled && (dsp->dsp_toggles.pressed_button == LOCK))
    {
        _beep();
    }

    if(cio->cio_states.target != _prev_cio_states.target)
    {
        const uint8_t liveTarget = cio->cio_states.target;
        const bool transitionWasActive = _target_set_in_progress;
        const uint8_t transitionGoal = _target_set_goal;
        const bool expectedTransitionValue = _targetSetTransitionExpected(liveTarget);

        if(transitionWasActive)
            _observeTargetSetTransition(liveTarget);

        if(transitionWasActive && expectedTransitionValue)
        {
            // Intermediate values are expected progress only. Do not copy them
            // into _web_target/_last_safe_target, otherwise 22->40 would stop at
            // the first observed 23/24/... value. Only the final goal is adopted.
            if(liveTarget == transitionGoal)
            {
                _web_target = liveTarget;
                _setLastSafeTargetFromCurrentUnit(liveTarget);
                _has_last_safe_states = true;
                _target_suspicious_since_ms = 0;
            }
        }
        else
        {
            // Wenn die Zieltemperatur direkt am Pumpendisplay geändert wurde,
            // muss dieser gültige Live-Wert auch wieder als Web-/Safe-Ziel übernommen
            // werden. Sonst kann die WebApp wegen des Target-Guards weiter den alten
            // _web_target/_last_safe_target anzeigen.
            if(!_targetLooksSuspicious(liveTarget))
            {
                _web_target = liveTarget;
                _setLastSafeTargetFromCurrentUnit(liveTarget);
                _has_last_safe_states = true;
                _target_suspicious_since_ms = 0;
            }
        }
    }

    if(
        cio->cio_states.unit   != _prev_cio_states.unit || 
        cio->cio_states.pump   != _prev_cio_states.pump || 
        cio->cio_states.heat   != _prev_cio_states.heat || 
        cio->cio_states.target != _prev_cio_states.target
      )
        _save_states_needed = true;

    Buttons _currbutton = dsp->dsp_toggles.pressed_button;
    if(_currbutton != _prevbutton && _currbutton != NOBTN)
    {
        _btn_sequence[0] = _btn_sequence[1];
        _btn_sequence[1] = _btn_sequence[2];
        _btn_sequence[2] = _btn_sequence[3];
        _btn_sequence[3] = _currbutton;
    }

    _prev_cio_states = cio->cio_states;
    _prev_dsp_states = dsp->dsp_states;
    _prevbutton = _currbutton;
    /* check changes from DSP 4W - go to antigodmode if someone presses a button*/
}

// return how many hours until pool is ready. (provided the heater is on)
float BWC::_estHeatingTime()
{
    int targetInC = cio->cio_states.target;
    if(!cio->cio_states.unit) targetInC = F2C(targetInC);
    if(_virtual_temp > targetInC) return -2;  //Already

    // float degAboveAmbient = _virtual_temp - (float)_ambient_temp;
    // float fraction = 1.0f - (degAboveAmbient - floor(degAboveAmbient));
    // int deltaTemp = targetInC - _virtual_temp;

    // //integrate the time needed to reach target
    // //how long to next integer temp
    // double coolingPerHour = degAboveAmbient / _R_COOLING;
    // double netRisePerHour;
    // netRisePerHour = _heating_degperhour - coolingPerHour;

    // double hoursRemaining = fraction / netRisePerHour;

    double degAboveAmbient;
    double deltaTemp = targetInC - _virtual_temp;
    double coolingPerHour;
    double netRisePerHour;
    double hoursRemaining = 0;
    //iterate up to target
    for(float i = 0; i <= deltaTemp; i += 0.01)
    {
        degAboveAmbient = _virtual_temp + i - _ambient_temp;
        coolingPerHour = degAboveAmbient / _R_COOLING;
        netRisePerHour = _heating_degperhour - coolingPerHour;
        if(netRisePerHour <= 0) return -1; //Never
        hoursRemaining += 0.01 / netRisePerHour;
    }

    if(hoursRemaining >= 0)
        return hoursRemaining;
    else 
        return -1; //Never
}

//virtual temp is always C in this code and will be converted when sending externally
void BWC::_calcVirtualTemp()
{
    //startup init
    if(millis() < 30000)
    {
        int tempInC = cio->cio_states.temperature;
        if(!cio->cio_states.unit) {
            tempInC = F2C(tempInC);
        }
        _virtual_temp_fix = tempInC;
        _virtual_temp = _virtual_temp_fix;
        _virtual_temp_fix_age = 0;
        return;
    }

    // calculate from last updated VTFix.
    double netRisePerHour;
    float degAboveAmbient = _virtual_temp - _ambient_temp;
    double coolingPerHour = degAboveAmbient / _R_COOLING;

    if(cio->cio_states.heatred)
    {
        netRisePerHour = _heating_degperhour - coolingPerHour;
    }
    else
    {
        netRisePerHour = - coolingPerHour;
    }
    double elapsed_hours = _virtual_temp_fix_age / 3600.0 / 1000.0;
    float newvt = _virtual_temp_fix + netRisePerHour * elapsed_hours;

    // clamp VT to +/- 1 from measured temperature if pump is running
    if(cio->cio_states.pump && ((millis()-_pump_change_timestamp_ms) > 5*60000))
    {
        float tempInC = cio->cio_states.temperature;
        float limit = 0.99;
        if(!cio->cio_states.unit)
        {
            tempInC = F2C(tempInC);
            limit = 1/1.8;
        }
        float dev = newvt-tempInC;
        if(dev > limit) dev = limit;
        if(dev < -limit) dev = -limit;
        newvt = tempInC + dev;
    }

    // Rebase start of calculation from new integer temperature
    if(int(_virtual_temp) != int(newvt))
    {
        _virtual_temp_fix = newvt;
        _virtual_temp_fix_age = 0;
    }
    _virtual_temp = newvt;

    /* Using Newtons law of cooling
        T(t) = Tenv + (T(0) - Tenv)*e^(-t/r)
        r = -t / ln( (T(t)-Tenv) / (T(0)-Tenv) )
        dT/dt = (T(t) - Tenv) / r
        ----------------------------------------
        T(t) : Temperature at time t
        Tenv : _ambient_temp (considered constant)
        T(0) : Temperature at time 0 (_virtual_temp_fix)
        e    : natural number 2,71828182845904
        r    : a constant we need to find out by measurements
    */

}

//Called on temp change
void BWC::_updateVirtualTempFix_ontempchange()
{
    int tempInC = cio->cio_states.temperature;
    float conversion = 1;
    if(!cio->cio_states.unit) {
        tempInC = F2C(tempInC);
        conversion = 1/1.8;
    }
    //Do not process if temperature changed > 1 degree (reading spikes)
    if(abs(_deltatemp) != 1) return;

    //readings are only valid if pump is running and has been running for 5 min.
    if(!cio->cio_states.pump || ((millis()-_pump_change_timestamp_ms) < 5*60000)) return;

    _virtual_temp = tempInC;
    _virtual_temp_fix = tempInC;
    _virtual_temp_fix_age = 0;
    /*
    update_coolingDegPerHourArray
    Measured temp has changed by 1 degree over a certain time
    1 degree/(temperature age in ms / 3600 / 1000)hours = 3 600 000 / temperature age in ms
    */

    // We can only know something about rate of change if we had continous cooling since last update
    // (Nobody messed with the heater during the 1 degree change)
    if(_heatred_change_timestamp_ms > _temp_change_timestamp_ms) return; //bugfix by @cobaltfish
    // rate of heating is not subject to change (fixed wattage and pool size) so do this only if cooling
    // and do not calibrate if bubbles has been on
    if(_vt_calibrated) return;
    if(cio->cio_states.heatred || cio->cio_states.bubbles || (_bubbles_change_timestamp_ms > _temp_change_timestamp_ms)) return;
    if(_deltatemp > 0 && _virtual_temp > _ambient_temp) return; //temp is rising when it should be falling. Bail out
    if(_deltatemp < 0 && _virtual_temp < _ambient_temp) return; //temp is falling when it should be rising. Bail out
    float degAboveAmbient = _virtual_temp - _ambient_temp;
    // can't calibrate if ambient ~ virtualtemp
    if(abs(degAboveAmbient) <= 1) return;
    _R_COOLING = ((millis()-_temp_change_timestamp_ms)/3600000.0) / log((conversion*degAboveAmbient) / (conversion*(degAboveAmbient + _deltatemp)));
    _vt_calibrated = true;
    _save_settings_needed = true;
}

//Called on heater state change
void BWC::_updateVirtualTempFix_onheaterchange()
{
    _virtual_temp_fix = _virtual_temp;
    _virtual_temp_fix_age = 0;
}

void BWC::print(const String &txt)
{
    dsp->text += txt;
}

// String BWC::getDebugData()
// {
//     String res = "from cio ";
//     res += cio->cio_states.toString();
//     res += "to dsp ";
//     res += dsp->dsp_states.toString();
//     res += "from dsp ";
//     res += dsp->dsp_toggles.toString();
//     res += "to cio ";
//     res += cio->cio_toggles.toString();
//     res += "BtnQLen: ";
//     res += cio->_button_que_len;
//     return res;
// }

void BWC::setAmbientTemperature(int64_t amb, bool unit)
{
    _ambient_temp = (int)amb;
    if(!unit) _ambient_temp = F2C(_ambient_temp);

    _virtual_temp_fix = _virtual_temp;
    _virtual_temp_fix_age = 0;
}

String BWC::getModel()
{
    return cio->getModel();
}

bool BWC::add_command(command_que_item command_item)
{
    _save_cmdq_needed = true;
    /* TODO: handle resetq in handlecommandque() instead!!! */
    // if(command_item.cmd == RESETQ)
    // {
    //     _command_que.clear();
    //     return true;
    // }
    if(command_item.cmd == SETREADY)
    {
        command_item.val = (int64_t)command_item.xtime; //Use val field to store the time to be ready
        command_item.xtime = 0; //And start checking now
        command_item.interval = 0;
    }
    //add parameters to _command_que[rows][parameter columns] and sort the array on xtime.
    _command_que.push_back(command_item);
    std::sort(_command_que.begin(), _command_que.end(), _compare_command);
    return true;
}

bool BWC::edit_command(uint8_t index, command_que_item command_item)
{
    // Safety: index == size() is already out of range. The previous ">"
    // check could reach vector::at(size) on an invalid edit request.
    if(index >= _command_que.size()) return false;
    _save_cmdq_needed = true;
    /* TODO: handle resetq in handlecommandque() instead!!! */
    // if(command_item.cmd == RESETQ)
    // {
    //     _command_que.clear();
    //     return true;
    // }
    if(command_item.cmd == SETREADY)
    {
        command_item.val = (int64_t)command_item.xtime; //Use val field to store the time to be ready
        command_item.xtime = 0; //And start checking now
        command_item.interval = 0;
    }
    //add parameters to _command_que[index] and sort the array on xtime.
    _command_que.at(index) = command_item;
    std::sort(_command_que.begin(), _command_que.end(), _compare_command);
    return true;
}

bool BWC::del_command(uint8_t index)
{
    if(index >= _command_que.size()) return false;
    _save_cmdq_needed = true;
    _command_que.erase(_command_que.begin()+index);
    return true;
}

//check for special button sequence
bool BWC::getBtnSeqMatch()
{
    if( _btn_sequence[0] == POWER &&
        _btn_sequence[1] == LOCK &&
        _btn_sequence[2] == TIMER &&
        _btn_sequence[3] == POWER)
    {
        return true;
    }
    return false;
}

void BWC::getJSONStates(String &rtn) {
        // Allocate a temporary JsonDocument
        // Don't forget to change the capacity to match your requirements.
        // Use arduinojson.org/assistant to compute the capacity.
    //feed the dog
    #ifdef ESP8266
    ESP.wdtFeed();
    #endif
    DynamicJsonDocument doc(1536);

    // Set the values in the document
    doc[F("CONTENT")] = F("STATES");
    doc[F("TIME")] = _timestamp_secs;
    doc[F("LCK")] = cio->cio_states.locked;
    doc[F("PWR")] = cio->cio_states.power;
    doc[F("UNT")] = cio->cio_states.unit;
    doc[F("AIR")] = cio->cio_states.bubbles;
    doc[F("GRN")] = cio->cio_states.heatgrn;
    doc[F("RED")] = cio->cio_states.heatred;
    doc[F("FLT")] = cio->cio_states.pump;
    doc[F("CH1")] = cio->cio_states.char1;
    doc[F("CH2")] = cio->cio_states.char2;
    doc[F("CH3")] = cio->cio_states.char3;
    doc[F("HJT")] = cio->cio_states.jets;
    doc[F("BRT")] = dsp->dsp_states.brightness;
    doc[F("ERR")] = cio->cio_states.error;
    doc[F("GOD")] = (uint8_t)cio->cio_states.godmode;
    // Anzeige/API wie in 3.0.0: TGT ist der echte zuletzt von der Pumpe gelesene Zielwert.
    // Der Guard-Wert wird nur zusätzlich ausgegeben und darf die Anzeige nicht maskieren.
    const uint8_t actualTargetForJson = cio->cio_states.target;
    const uint8_t guardedTargetForJson = _guardedTargetForSave();
    doc[F("TGT")] = actualTargetForJson;
    doc[F("TGTRAW")] = actualTargetForJson;
    doc[F("TGTG")] = guardedTargetForJson;
    doc[F("TMP")] = cio->cio_states.temperature;
    doc[F("VTMC")] = _virtual_temp;
    doc[F("VTMF")] = C2F(_virtual_temp);
    doc[F("AMBC")] = _ambient_temp;
    doc[F("AMBF")] = round(C2F(_ambient_temp));
    if(cio->cio_states.unit)
    {
        //celsius
        doc[F("AMB")] = _ambient_temp;
        doc[F("VTM")] = _virtual_temp;
        doc[F("TGTC")] = actualTargetForJson;
        doc[F("TMPC")] = cio->cio_states.temperature;
        doc[F("TGTF")] = round(C2F((float)actualTargetForJson));
        doc[F("TMPF")] = round(C2F((float)cio->cio_states.temperature));
        // doc[F("VTMF")] = C2F(_virtual_temp);
    }
    else
    {
        //farenheit
        doc[F("AMB")] = round(C2F(_ambient_temp));
        doc[F("VTM")] = C2F(_virtual_temp);
        doc[F("TGTF")] = actualTargetForJson;
        doc[F("TMPF")] = cio->cio_states.temperature;
        doc[F("TGTC")] = round(F2C((float)actualTargetForJson));
        doc[F("TMPC")] = round(F2C((float)cio->cio_states.temperature));
        // doc[F("VTMC")] = _virtual_temp;
    }

    // Serialize JSON to string
    if (serializeJson(doc, rtn) == 0) {
        rtn = F("{\"error\": \"Failed to serialize states\"}");
    }
}

void BWC::getJSONTimes(String &rtn) {
    // Allocate a temporary JsonDocument
    // Don't forget to change the capacity to match your requirements.
    // Use arduinojson.org/assistant to compute the capacity.
    //feed the dog
    #ifdef ESP8266
    ESP.wdtFeed();
    #endif
    DynamicJsonDocument doc(1024);

    // Set the values in the document
    doc[F("CONTENT")] = F("TIMES");
    doc[F("TIME")] = _timestamp_secs;
    doc[F("CLTIME")] = _cl_timestamp_s;
    doc[F("FREP")] = _filter_replace_timestamp_s;
    doc[F("FRIN")] = _filter_rinse_timestamp_s;
    doc[F("FCLE")] = _filter_clean_timestamp_s;
    doc[F("UPTIME")] = _uptime + _uptime_ms/1000;
    doc[F("PUMPTIME")] = _pumptime + _pumptime_ms/1000;
    doc[F("HEATINGTIME")] = _heatingtime + _heatingtime_ms/1000;
    doc[F("AIRTIME")] = _airtime + _airtime_ms/1000;
    doc[F("JETTIME")] = _jettime + _jettime_ms/1000;
    doc[F("COST")] = _energy_cost;
    doc[F("FREPI")] = _filter_replace_interval;
    doc[F("FRINI")] = _filter_rinse_interval;
    doc[F("FCLEI")] = _filter_clean_interval;
    doc[F("CLINT")] = _cl_interval;
    doc[F("KWH")] = _energy_total_kWh;
    doc[F("KWHD")] = _energy_daily_Ws / 3600000.0; //Ws -> kWh
    doc[F("WATT")] = _energy_power_W;
    float t2r = _estHeatingTime();
    String t2r_string = F("Nicht bereit");
    if(t2r == -2) t2r_string = F("Bereit");
    if(t2r == -1) t2r_string = F("Niemals");
    doc[F("T2R")] = t2r;
    doc[F("RS")] = t2r_string;
    String s;
    s.reserve(256);
    s = cio->debug();
    s += F(" || ");
    s += dsp->debug();
    doc[F("DBG")] = s;
    //cio->clk_per = 1000;  //reset minimum clock period

    // Serialize JSON to string
    if (serializeJson(doc, rtn) == 0) {
        rtn = F("{\"error\": \"Failed to serialize times\"}");
    }
}

void BWC::getJSONSettings(String &rtn){
    // Allocate a temporary JsonDocument
    // Don't forget to change the capacity to match your requirements.
    // Use arduinojson.org/assistant to compute the capacity.
    //feed the dog
    #ifdef ESP8266
    ESP.wdtFeed();
    #endif
    DynamicJsonDocument doc(1024);

    // Set the values in the document
    doc[F("CONTENT")] = F("SETTINGS");
    doc[F("PRICE")] = _price;
    doc[F("FREPI")] = _filter_replace_interval;
    doc[F("FRINI")] = _filter_rinse_interval;
    doc[F("FCLEI")] = _filter_clean_interval;
    doc[F("CLINT")] = _cl_interval;
    doc[F("AUDIO")] = _audio_enabled;
    #ifdef ESP8266
    doc[F("REBOOTINFO")] = ESP.getResetReason();
    #endif
    doc[F("REBOOTTIME")] = reboot_time_t;
    doc[F("RESTORE")] = _restore_states_on_start;
    doc[F("MODEL")] = cio->getModel();
    doc[F("NOTIFY")] = _notify;
    doc[F("NOTIFTIME")] = _notification_time;
    doc[F("POOLCAP")] = _pool_capacity;
    doc[F("VTCAL")] = _vt_calibrated;

    doc[F("LCK")] = dsp->EnabledButtons[LOCK];
    doc[F("TMR")] = dsp->EnabledButtons[TIMER];
    doc[F("AIR")] = dsp->EnabledButtons[BUBBLES];
    doc[F("UNT")] = dsp->EnabledButtons[UNIT];
    doc[F("HTR")] = dsp->EnabledButtons[HEAT];
    doc[F("FLT")] = dsp->EnabledButtons[PUMP];
    doc[F("DN")] = dsp->EnabledButtons[DOWN];
    doc[F("UP")] = dsp->EnabledButtons[UP];
    doc[F("PWR")] = dsp->EnabledButtons[POWER];
    doc[F("HJT")] = dsp->EnabledButtons[HYDROJETS];

    // Serialize JSON to string
    if (serializeJson(doc, rtn) == 0) {
        rtn = F("{\"error\": \"Failed to serialize settings\"}");
    }
}

String BWC::getJSONCommandQueue(){
    //feed the dog
    #ifdef ESP8266
    ESP.wdtFeed();
    #endif
    DynamicJsonDocument doc(1024);
    // Set the values in the document
    doc[F("LEN")] = _command_que.size();
    for(unsigned int i = 0; i < _command_que.size(); i++){
        doc[F("CMD")][i] = _command_que[i].cmd;
        doc[F("VALUE")][i] = _command_que[i].val;
        doc[F("XTIME")][i] = _command_que[i].xtime;
        doc[F("INTERVAL")][i] = _command_que[i].interval;
        doc[F("TXT")][i] = _command_que[i].text;
    }

    // Serialize JSON to file
    String jsonmsg;
    if (serializeJson(doc, jsonmsg) == 0) {
        jsonmsg = F("{\"error\": \"Failed to serialize cmdq\"}");
    }
    return jsonmsg;
}

/*TODO:*/
uint8_t BWC::getState(int state){
    // return cio->getState(state);
    return 0;
}

void BWC::getButtonName(String &rtn) {
    rtn = ButtonNames[dsp->dsp_toggles.pressed_button];
}

Buttons BWC::getButton()
{
    return dsp->dsp_toggles.pressed_button;
}

void BWC::setJSONSettings(const String& message){
    //feed the dog
    // ESP.wdtFeed();
    DynamicJsonDocument doc(1024);

    // Deserialize the JSON document
    DeserializationError error = deserializeJson(doc, message);
    if (error) {
        // Serial.println(F("Failed to read config file"));
        return;
    }

    // Copy existing values from the JsonDocument to the variables
    _price = doc[F("PRICE")] | _price;
    _filter_replace_interval = doc[F("FREPI")] | _filter_replace_interval;
    _filter_rinse_interval = doc[F("FRINI")] | _filter_rinse_interval;
    _filter_clean_interval = doc[F("FCLEI")] | _filter_clean_interval;
    _cl_interval = doc[F("CLINT")] | _cl_interval;
    _audio_enabled = doc[F("AUDIO")] | _audio_enabled;
    _restore_states_on_start = doc[F("RESTORE")] | _restore_states_on_start;
    _notify = doc[F("NOTIFY")] | _notify;
    _notification_time = doc[F("NOTIFTIME")] | _notification_time;
    if(_notification_time < 1 || _notification_time > 1000)
    {
        _notification_time = 32;
    }
    _next_notification_time = _notification_time;
    _vt_calibrated = doc[F("VTCAL")] | _vt_calibrated;
    if(doc.containsKey(F("POOLCAP"))) { int v = doc[F("POOLCAP")]; if(v >= 100 && v <= 3000) _pool_capacity = v; }
    dsp->EnabledButtons[LOCK] = doc[F("LCK")] | dsp->EnabledButtons[LOCK];
    dsp->EnabledButtons[TIMER] = doc[F("TMR")] | dsp->EnabledButtons[TIMER];
    dsp->EnabledButtons[BUBBLES] = doc[F("AIR")] | dsp->EnabledButtons[BUBBLES];
    dsp->EnabledButtons[UNIT] = doc[F("UNT")] | dsp->EnabledButtons[UNIT];
    dsp->EnabledButtons[HEAT] = doc[F("HTR")] | dsp->EnabledButtons[HEAT];
    dsp->EnabledButtons[PUMP] = doc[F("FLT")] | dsp->EnabledButtons[PUMP];
    dsp->EnabledButtons[DOWN] = doc[F("DN")] | dsp->EnabledButtons[DOWN];
    dsp->EnabledButtons[UP] = doc[F("UP")] | dsp->EnabledButtons[UP];
    dsp->EnabledButtons[POWER] = doc[F("PWR")] | dsp->EnabledButtons[POWER];
    dsp->EnabledButtons[HYDROJETS] = doc[F("HJT")] | dsp->EnabledButtons[HYDROJETS];
    saveSettings();
}

bool BWC::newData(){
    bool result = _new_data_available;
    _new_data_available = false;
    return result;
}

void BWC::_updateTimes(){
    uint32_t now = millis();
    static uint32_t prevtime = now;
    int elapsedtime_ms = now-prevtime;
    prevtime = now;
    // //(some of) these age-counters resets when the state changes
    // for(unsigned int i = 0; i < cio->getSizeofStates(); i++)
    // {
    //     cio->setStateAge(i, cio->getStateAge(i) + elapsedtime_ms);
    // }
    _virtual_temp_fix_age += elapsedtime_ms;

    if (elapsedtime_ms < 0) return; //millis() rollover every 24,8 days
    if(cio->cio_states.heatred){
        _heatingtime_ms += elapsedtime_ms;
    }
    if(cio->cio_states.pump){
        _pumptime_ms += elapsedtime_ms;
    }
    if(cio->cio_states.bubbles){
        _airtime_ms += elapsedtime_ms;
    }
    if(cio->cio_states.jets){
        _jettime_ms += elapsedtime_ms;
    }
    _uptime_ms += elapsedtime_ms;


    if(_uptime_ms > 1000000000){
        _heatingtime += _heatingtime_ms/1000;
        _pumptime += _pumptime_ms/1000;
        _airtime += _airtime_ms/1000;
        _jettime += _jettime_ms/1000;
        _uptime += _uptime_ms/1000;
        // Keep the sub-second remainder instead of discarding it.
        _heatingtime_ms %= 1000;
        _pumptime_ms %= 1000;
        _airtime_ms %= 1000;
        _jettime_ms %= 1000;
        _uptime_ms %= 1000;
    }

    if(_override_dsp_brt_timer > 0) _override_dsp_brt_timer -= elapsedtime_ms; //counts down to or below zero

    // Current electrical load based on the configured SmartAndRelax power levels.
    _energy_power_W = cio->cio_states.heatred * cio->getHeaterPower();
    _energy_power_W += cio->cio_states.pump * cio->getPowerLevels().PUMPPOWER;
    _energy_power_W += cio->cio_states.bubbles * cio->getPowerLevels().AIRPOWER;
    _energy_power_W += cio->getPowerLevels().IDLEPOWER;
    _energy_power_W += cio->cio_states.jets * cio->getPowerLevels().JETPOWER;

    // Convert the power used during this real time interval to energy.
    // kWh = W * ms / 3,600,000,000
    _energy_total_kWh += static_cast<double>(_energy_power_W) * elapsedtime_ms / 3600000000.0;
    _energy_daily_Ws += static_cast<double>(_energy_power_W) * elapsedtime_ms / 1000.0;

                                                                              
                                                                            
                                                                              
    _energy_cost = _energy_total_kWh * static_cast<double>(_price);

    if(_notes.size())
    {
        dsp->audiofrequency = _notes.back().frequency_hz;
        _note_duration += elapsedtime_ms;
        if(_note_duration >= _notes.back().duration_ms)
        {
            _note_duration -= _notes.back().duration_ms;
            _notes.pop_back();
            _note_duration = 0;
        }
    }
    else
    {
        dsp->audiofrequency = 0;
    }
}

/*          */
/* LOADERS  */
/*          */

bool BWC::_loadHardware(Models& cioNo, Models& dspNo, int pins[], std::optional<Power>& power_levels)
{
    File file = LittleFS.open(F("/hwcfg.json"), "r");
    if (!file)
    {
        // Serial.println(F("Failed to open hwcfg.json"));
        return false;
    }
    // DynamicJsonDocument doc(256);
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, file);
    if (error) {
        // Serial.println(F("Failed to read settings.txt"));
        file.close();
        return false;
    }
    file.close();
    cioNo = doc[F("cio")];
    dspNo = doc[F("dsp")];

    if(doc[F("hasTempSensor")].as<int>() == 1)
    {
        hasTempSensor = true;
    }

    String pcbname = doc[F("pcb")].as<String>();
    // int pins[7];
    #ifdef ESP8266
    int DtoGPIO[] = {D0, D1, D2, D3, D4, D5, D6, D7, D8};
    #endif
    for(int i = 0; i < 8; i++)
    {
        pins[i] = doc[F("pins")][i];
    #ifdef ESP8266
        pins[i] = DtoGPIO[pins[i]];
    #endif
    }

    const auto pwr_levels_json = doc[F("pwr_levels")];
    if (pwr_levels_json[F("override")].as<bool>()) {
        power_levels.emplace(
            Power{
                .HEATERPOWER_STAGE1 = pwr_levels_json[F("heater_stage1")].as<int>(),
                .HEATERPOWER_STAGE2 = pwr_levels_json[F("heater_stage2")].as<int>(),
                .PUMPPOWER = pwr_levels_json[F("pump")].as<int>(),
                .AIRPOWER = pwr_levels_json[F("air")].as<int>(),
                .IDLEPOWER = pwr_levels_json[F("idle")].as<int>(),
                .JETPOWER = pwr_levels_json[F("jet")].as<int>(),
            }
        );
    }

    return true;
}

void BWC::reloadSettings(){
    _loadSettings();
    return;
}

void BWC::_loadSettings(){
    File file = LittleFS.open(F("/settings.json"), "r");
    if (!file) {
        // Serial.println(F("Failed to load settings.json"));
        return;
    }
    DynamicJsonDocument doc(1024);

    // Deserialize the JSON document
    DeserializationError error = deserializeJson(doc, file);
    if (error) {
        // Serial.println(F("Failed to deser. settings.json"));
        file.close();
        return;
    }

    // Copy values from the JsonDocument to the variables
    _cl_timestamp_s = doc[F("CLTIME")];
    _filter_replace_timestamp_s = doc[F("FREP")];
    _filter_rinse_timestamp_s = doc[F("FRIN")];
    _filter_clean_timestamp_s = doc[F("FCLE")];
    _uptime = doc[F("UPTIME")];
    _pumptime = doc[F("PUMPTIME")];
    _heatingtime = doc[F("HEATINGTIME")];
    _airtime = doc[F("AIRTIME")];
    _jettime = doc[F("JETTIME")];
    _price = doc[F("PRICE")];
    _filter_replace_interval = doc[F("FREPI")] | _filter_replace_interval;
    _filter_rinse_interval = doc[F("FRINI")] | _filter_rinse_interval;
    _filter_clean_interval = doc[F("FCLEI")] | _filter_clean_interval;
    _cl_interval = doc[F("CLINT")];
    _audio_enabled = doc[F("AUDIO")];
    _notify = doc[F("NOTIFY")];
    _notification_time = doc[F("NOTIFTIME")] | _notification_time;
    if(_notification_time < 1 || _notification_time > 1000)
    {
        _notification_time = 32;
    }
    _next_notification_time = _notification_time;
    _energy_total_kWh = doc[F("KWH")];
    _energy_daily_Ws = doc[F("KWHD")];
    _energy_cost = doc[F("COST")];
    _restore_states_on_start = doc[F("RESTORE")];
    _R_COOLING = doc[F("R")] | 40.0f; //else use default
    _ambient_temp = doc[F("AMB")] | 20;
    _dsp_brightness = doc[F("BRT")] | 7;
    _vt_calibrated = doc[F("VTCAL")] | false;
    _pool_capacity = doc[F("POOLCAP")] | 700;
    if(_pool_capacity < 100 || _pool_capacity > 3000) _pool_capacity = 700;

    dsp->EnabledButtons[LOCK] = doc[F("LCK")];
    dsp->EnabledButtons[TIMER] = doc[F("TMR")];
    dsp->EnabledButtons[BUBBLES] = doc[F("AIR")];
    dsp->EnabledButtons[UNIT] = doc[F("UNT")];
    dsp->EnabledButtons[HEAT] = doc[F("HTR")];
    dsp->EnabledButtons[PUMP] = doc[F("FLT")];
    dsp->EnabledButtons[DOWN] = doc[F("DN")];
    dsp->EnabledButtons[UP] = doc[F("UP")];
    dsp->EnabledButtons[POWER] = doc[F("PWR")];
    dsp->EnabledButtons[HYDROJETS] = doc[F("HJT")];

    file.close();
}

void BWC::_restoreStates() {
    if(!_restore_states_on_start) return;
    File file = LittleFS.open(F("states.txt"), "r");
    if (!file) {
        // Serial.println(F("Failed to read states.txt"));
        return;
    }
    // DynamicJsonDocument doc(512);
    StaticJsonDocument<512> doc;
    // Deserialize the JSON document
    DeserializationError error = deserializeJson(doc, file);
    if (error) {
        // Serial.println(F("Failed to deserialize states.txt"));
        file.close();
        return;
    }

    uint8_t unt = doc[F("UNT")];
    uint8_t flt = doc[F("FLT")];
    uint8_t htr = doc[F("HTR")];
    uint8_t tgt = doc[F("TGT")] | 20;
    uint8_t god = doc[F("GOD")] ;

    // Wenn ein persistenter Safe-State vorhanden ist, darf ein alter/falscher
    // OFF-Wert aus states.txt den letzten sicheren EIN-Zustand nicht überstimmen.
    if(_has_last_safe_states)
    {
        flt = _last_safe_pump;
        htr = _last_safe_heat;
        const uint8_t safeTgtForRestoreUnit = _convertTargetToUnit(_last_safe_target, _last_safe_unit, unt ? 1 : 0);

        // Nicht nur bei unplausiblem TGT ersetzen:
        // Nach einem Crash kann states.txt zwar 28 enthalten und Diag/WebUI damit
        // korrekt aussehen, während die echte Pumpeneinheit intern noch 38 hält.
        // Deshalb wird beim Start immer der letzte sichere Zielwert verwendet und
        // unten zusätzlich ein GETTARGET vor SETTARGET eingeplant.
        tgt = safeTgtForRestoreUnit;
        god = _last_safe_god;
    }

    command_que_item item;
    item.cmd = SETGODMODE;
    item.val = god;
    item.xtime = 0;
    item.interval = 0;
    item.text = "";
    add_command(item);
    item.cmd = SETUNIT;
    item.val = unt;
    item.xtime = 0;
    item.interval = 0;
    item.text = "";
    add_command(item);
    item.cmd = SETPUMP;
    item.val = flt;
    item.xtime = 0;
    item.interval = 0;
    item.text = "";
    add_command(item);
    item.cmd = SETHEATER;
    item.val = htr;
    item.xtime = 0;
    item.interval = 0;
    item.text = "";
    add_command(item);
    // Vor SETTARGET den echten Pumpen-Zielwert aktiv neu einlesen.
    // Das verhindert, dass ein gecachter Zielwert im Modul einen abweichenden
    // Zielwert in der Pumpeneinheit verdeckt.
    item.cmd = GETTARGET;
    item.val = 0;
    item.xtime = 0;
    item.interval = 0;
    item.text = "";
    add_command(item);

    item.cmd = SETTARGET;
    item.val = tgt;
    item.xtime = 0;
    item.interval = 0;
    item.text = "";
    add_command(item);
    // Serial.println(F("Restoring states"));
    file.close();
}

void BWC::reloadCommandQueue(){
    loadCommandQueue();
}

void BWC::loadCommandQueue(){
    File file = LittleFS.open(F("/cmdq.json"), "r");
    if (!file) {
        // Serial.println(F("Failed to read cmdq.json"));
        return;
    }

    DynamicJsonDocument doc(1024);
    // Deserialize the JSON document
    DeserializationError error = deserializeJson(doc, file);
    if (error) {
        // Serial.println(F("Failed to deserialize cmdq.json"));
        file.close();
        return;
    }
    _command_que.clear();
    // Set the values in the variables
    for(int i = 0; i < doc[F("LEN")]; i++){
        command_que_item item;
        item.cmd = doc[F("CMD")][i];
        item.val = doc[F("VALUE")][i];
        item.xtime = doc[F("XTIME")][i];
        item.interval = doc[F("INTERVAL")][i];
        String s = doc[F("TXT")][i] | "";
        item.text = s;
        while((item.interval > 0) && (item.xtime < (uint64_t)time(nullptr))) item.xtime += item.interval;
        _command_que.push_back(item);
    }
    file.close();
    std::sort(_command_que.begin(), _command_que.end(), _compare_command);
}

/*          */
/* SAVERS   */
/*          */

void BWC::saveRebootInfo(){
    File file = LittleFS.open(F("bootlog.txt"), "a");
    if (!file) {
        // Serial.println(F("Failed to save bootlog.txt"));
        return;
    }

    // DynamicJsonDocument doc(1024);
    StaticJsonDocument<256> doc;

    // Set the values in the document
    #ifdef ESP8266
    doc[F("BOOTINFO")] = ESP.getResetReason() + " " + reboot_time_str;
    #endif

    // Serialize JSON to file
    if (serializeJson(doc, file) == 0) {
        // Serial.println(F("Failed to write bootlog.txt"));
    }
    file.println();
    file.close();
}

void BWC::_saveStates() {
    #ifdef ESP8266
    ESP.wdtFeed();
    #endif
    _save_states_needed = false;

    if(cloudPollingGuardActive())
    {
        _cloud_poll_guard_save_skip_count++;
        _state_guard_skip_count++;
        return;
    }

    if(_shouldBlockUnsafeStateSave())
    {
        _state_guard_skip_count++;
        _enforceSafeLiveState("blocked unsafe save");
        _enforceSafeTargetState("blocked unsafe save");
        return;
    }

    _enforceSafeTargetState("save states");

    File file = LittleFS.open(F("states.txt"), "w");
    if (!file) {
        return;
    }

    StaticJsonDocument<256> doc;

    doc[F("UNT")] = cio->cio_states.unit;
    doc[F("HTR")] = cio->cio_states.heat;
    doc[F("FLT")] = cio->cio_states.pump;
    doc[F("TGT")] = _guardedTargetForSave();
    doc[F("GOD")] = (uint8_t)cio->cio_states.godmode;

    serializeJson(doc, file);
    file.close();

    _updateLastKnownSafeStates();
}

void BWC::_saveCommandQueue()
{
    // --- 1) Rate-Limit / Debounce: max 1x alle 3000 ms schreiben ---
    const uint32_t now = millis();
    const uint32_t MIN_SAVE_GAP_MS = 3000;

    if ((uint32_t)(now - g_cmdq_last_save_ms) < MIN_SAVE_GAP_MS) {
        // Noch zu früh -> später nochmal versuchen, Flag bleibt true
        return;
    }

    // --- 2) "Nicht speichern" Sonderfall: Instant-Reboot ganz vorne ---
    // WICHTIG: Das muss VOR dem File-open passieren!
    if (_command_que.size() &&
        _command_que[0].cmd == REBOOTESP &&
        _command_que[0].interval == 0)
    {
        _save_cmdq_needed = false; // damit er nicht jede Loop wieder versucht
        return;
    }

    // --- 3) JSON bauen und Hash vergleichen (nur schreiben, wenn geändert) ---
    DynamicJsonDocument doc(1024);
    doc[F("LEN")] = _command_que.size();

    for (unsigned int i = 0; i < _command_que.size(); i++) {
        doc[F("CMD")][i]      = _command_que[i].cmd;
        doc[F("VALUE")][i]    = _command_que[i].val;
        doc[F("XTIME")][i]    = _command_que[i].xtime;
        doc[F("INTERVAL")][i] = _command_que[i].interval;
        doc[F("TXT")][i]      = _command_que[i].text;
    }

    String json;
    json.reserve(768);
    if (serializeJson(doc, json) == 0) {
        // konnte nicht serialisieren -> später erneut versuchen
        return;
    }

    const uint32_t h = fnv1a32((const uint8_t*)json.c_str(), json.length());
    if (h == g_cmdq_last_hash) {
        // Inhalt identisch -> kein Flash-Write nötig
        _save_cmdq_needed = false;
        g_cmdq_last_save_ms = now; // trotzdem "beruhigen"
        return;
    }

    // --- 4) Jetzt erst File schreiben ---
    File file = LittleFS.open(F("/cmdq.json"), "w");
    if (!file) {
        Serial.println(F("Failed to save cmdq.json"));
        return;
    }

    Serial.println(F("Writing cmdq.json"));

    size_t written = file.print(json);
    file.close();

    if (written == 0) {
        // Schreiben fehlgeschlagen -> später erneut versuchen
        Serial.println(F("cmdq.json write FAILED"));
        return;
    }

    Serial.println(F("Done!"));

    // --- 5) Erfolg: Flags/Tracker updaten ---
    g_cmdq_last_hash = h;
    g_cmdq_last_save_ms = now;
    _save_cmdq_needed = false;
}


void BWC::saveSettings(){
    //kill the dog
    // ESP.wdtDisable();
    #ifdef ESP8266
    ESP.wdtFeed();
    #endif
    _save_settings_needed = false;
    File file = LittleFS.open(F("settings.json"), "w");
    if (!file) {
        // Serial.println(F("Failed to save settings.json"));
        return;
    }

    DynamicJsonDocument doc(1024);
    _heatingtime += _heatingtime_ms/1000;
    _pumptime += _pumptime_ms/1000;
    _airtime += _airtime_ms/1000;
    _jettime += _jettime_ms/1000;
    _uptime += _uptime_ms/1000;
    // Preserve the fractions in RAM; only complete seconds are persisted.
    _heatingtime_ms %= 1000;
    _pumptime_ms %= 1000;
    _airtime_ms %= 1000;
    _jettime_ms %= 1000;
    _uptime_ms %= 1000;
    // Set the values in the document
    doc[F("CLTIME")] = _cl_timestamp_s;
    doc[F("FREP")] = _filter_replace_timestamp_s;
    doc[F("FRIN")] = _filter_rinse_timestamp_s;
    doc[F("FCLE")] = _filter_clean_timestamp_s;
    doc[F("UPTIME")] = _uptime;
    doc[F("PUMPTIME")] = _pumptime;
    doc[F("HEATINGTIME")] = _heatingtime;
    doc[F("AIRTIME")] = _airtime;
    doc[F("JETTIME")] = _jettime;
    doc[F("PRICE")] = _price;
    doc[F("FREPI")] = _filter_replace_interval;
    doc[F("FRINI")] = _filter_rinse_interval;
    doc[F("FCLEI")] = _filter_clean_interval;
    doc[F("CLINT")] = _cl_interval;
    doc[F("AUDIO")] = _audio_enabled;
    doc[F("KWH")] = _energy_total_kWh;
    doc[F("KWHD")] = _energy_daily_Ws;
    doc[F("COST")] = _energy_cost;
    // doc[F("SAVETIME")] = DateTime.format(DateFormatter::SIMPLE);
    doc[F("RESTORE")] = _restore_states_on_start;
    doc[F("R")] = _R_COOLING;
    doc[F("AMB")] = _ambient_temp;
    doc[F("POOLCAP")] = _pool_capacity;
    doc[F("BRT")] = _dsp_brightness;
    doc[F("NOTIFY")] = _notify;
    doc[F("NOTIFTIME")] = _notification_time;
    doc[F("VTCAL")] = _vt_calibrated;
    doc[F("LCK")] = dsp->EnabledButtons[LOCK];
    doc[F("TMR")] = dsp->EnabledButtons[TIMER];
    doc[F("AIR")] = dsp->EnabledButtons[BUBBLES];
    doc[F("UNT")] = dsp->EnabledButtons[UNIT];
    doc[F("HTR")] = dsp->EnabledButtons[HEAT];
    doc[F("FLT")] = dsp->EnabledButtons[PUMP];
    doc[F("DN")] = dsp->EnabledButtons[DOWN];
    doc[F("UP")] = dsp->EnabledButtons[UP];
    doc[F("PWR")] = dsp->EnabledButtons[POWER];
    doc[F("HJT")] = dsp->EnabledButtons[HYDROJETS];

    // Serialize JSON to file
    if (serializeJson(doc, file) == 0) {
        // Serial.println(F("Failed to write json to settings.json"));
    }
    file.close();
    //revive the dog
    // ESP.wdtEnable(0);
}

//save out debug text to file "debug.txt" on littleFS
void BWC::saveDebugInfo(const String& s){
    File file = LittleFS.open(F("debug.txt"), "a");
    if (!file) {
        // Serial.println(F("Failed to save debug.txt"));
        return;
    }

    DynamicJsonDocument doc(1024);

    // Set the values in the document
    doc[F("timestamp")] = time(nullptr);
    doc[F("message")] = s;
    // Serialize JSON to file
    if (serializeJson(doc, file) == 0) {
        // Serial.println(F("Failed to write debug.txt"));
    }
    file.close();
}

/* SOUND */

/*temporary function to render some soundfiles*/
// void BWC::_save_melody(const String& filename)
// {
//     File file = LittleFS.open(filename, "w");
//     if (!file) return;
//     sNote n = {1000, 500};
//     file.write((byte*)&n, sizeof(n));
//     file.close();
// }

bool BWC::_load_melody_json(const String& filename)
{
    if(_notes.size() || !_audio_enabled){
        // Serial.println("Q busy");
        return false;
    } 
    File file = LittleFS.open(filename, "r");
    if (!file){
        // Serial.println("file error");
        return false; 
    } 
    int beat_period;
    float note_duty_cycle;
    sNote n;

    /*new file format: 
    beat period
    note duty cycle
    frequency
    duration (fraction of beat_period)
    frequency
    duration
    ...eof
    */
   _notes.reserve(128);
    String s = file.readStringUntil('\n');
    beat_period = s.toInt();
    s = file.readStringUntil('\n');
    note_duty_cycle = s.toFloat();
    while(file.available())
    {
        s = file.readStringUntil('\n');
        n.frequency_hz = s.toInt();
        s = file.readStringUntil('\n');
        n.duration_ms = beat_period * s.toFloat();
        n.duration_ms *= note_duty_cycle;
        _notes.push_back(n);
        /*add a little break between the notes (will be placed before each note due to reversing)*/
        n.frequency_hz = 0;
        n.duration_ms = beat_period * s.toFloat();
        n.duration_ms *= (1-note_duty_cycle);
        _notes.push_back(n);
    }

    std::reverse(_notes.begin(), _notes.end());
    file.close();

    return true;
}

// void BWC::_add_melody(const String &filename)
// {
//     if(_notes.size() || !_audio_enabled) return;
//     File file = LittleFS.open(filename, "r");
//     if (!file) return;
//     while(file.available())
//     {
//         sNote n;
//         file.readBytes((char*)&n, sizeof(n));
//         _notes.push_back(n);
//     }
//     file.close();
//     /* We read and erase from the back of the vector (faster) so if notes are stored in the natural order we need to reverse*/
//     std::reverse(_notes.begin(), _notes.end());
// }

void BWC::_sweepdown()
{
    if(_notes.size() || !_audio_enabled) return;
    _notes.reserve(128);
    for(int i = 0; i < 128; i++)
    {
        sNote n;
        n.duration_ms = 2;
        n.frequency_hz = 1000 + 8*i;
        _notes.push_back(n);
    }
}

void BWC::_sweepup()
{
    if(_notes.size() || !_audio_enabled) return;
    _notes.reserve(128);
    for(int i = 0; i < 128; i++)
    {
        sNote n;
        n.duration_ms = 2;
        n.frequency_hz = 2000 - 8*i;
        _notes.push_back(n);
    }
}

void BWC::_beep()
{
    if(_notes.size() || !_audio_enabled) return;
    sNote n;
    n.duration_ms = 50;
    n.frequency_hz = 2400;
    _notes.push_back(n);
    n.duration_ms = 50;
    n.frequency_hz = 800;
    _notes.push_back(n);
}

void BWC::_accord()
{
    if(_notes.size() || !_audio_enabled) return;
    sNote n;
    for(int i = 0; i < 5; i++)
    {
        n.duration_ms = 10;
        n.frequency_hz = NOTE_C6;
        _notes.push_back(n);
        n.duration_ms = 10;
        n.frequency_hz = NOTE_E6;
        _notes.push_back(n);
    }
}

// --- Smart Schedule (predictive heating) ---
bool BWC::setSmartSchedule(uint64_t target_time, uint8_t target_temp, bool keep_heater_on, int pool_capacity)
{
    const uint64_t now = (uint64_t)time(nullptr);
    if(now < 57600ULL || target_time <= now || target_temp < 20 || target_temp > 40) return false;
    if(pool_capacity != 0) { if(pool_capacity < 100 || pool_capacity > 3000) return false; _pool_capacity = pool_capacity; _save_settings_needed = true; }
    _smart_schedule = smart_schedule_t();
    _smart_schedule.active = true;
    _smart_schedule.target_time = target_time;
    _smart_schedule.target_temp = target_temp;
    _smart_schedule.keep_heater_on = keep_heater_on;
    uint8_t cur = cio ? cio->cio_states.temperature : 20;
    if(cio && !cio->cio_states.unit) cur = (uint8_t)round(F2C(cur));
    _smart_schedule.last_heating_estimate = _calculateHeatingTime(cur, target_temp);
    _save_smartschedule_needed = true; _new_data_available = true;
    return true;
}

bool BWC::updateSmartScheduleKeepHeaterOn(bool keep)
{
    if(!_smart_schedule.active) return false;
    _smart_schedule.keep_heater_on = keep; _save_smartschedule_needed = true; _new_data_available = true; return true;
}

void BWC::_resetSmartScheduleState()
{
    _smart_schedule = smart_schedule_t(); _save_smartschedule_needed = true; _new_data_available = true;
}

void BWC::cancelSmartSchedule()
{
    if(_smart_schedule.heater_started_by_schedule && cio && cio->cio_states.heat) { command_que_item i{0,0,SETHEATER,0,""}; add_command(i); }
    if(_smart_schedule.temp_reading_started_pump && cio && cio->cio_states.pump && !cio->cio_states.heat) { command_que_item i{0,0,SETPUMP,0,""}; add_command(i); }
    _resetSmartScheduleState();
}

float BWC::_calculateHeatingTime(uint8_t current_temp, uint8_t target_temp)
{
    if(current_temp >= target_temp) return 0.0f;
    const float heaterPwr=2000.0f, heatLoss=4.85f, cp=1.163f;
    const float avg=((float)current_temp+(float)target_temp)/2.0f;
    float efficiency=0.99f-((avg-(float)_ambient_temp)*0.005f);
    if(efficiency<0.10f) efficiency=0.10f; if(efficiency>0.99f) efficiency=0.99f;
    const float net=heaterPwr*efficiency-heatLoss*(avg-(float)_ambient_temp);
    if(net<=0.0f) return 999.0f;
    return ((float)_pool_capacity*cp*((float)target_temp-(float)current_temp))/net;
}

void BWC::_startAccurateTempReading()
{
    if(!cio) return;
    _smart_schedule.temp_reading_started_pump = false;
    if(!cio->cio_states.pump) { command_que_item i{1,0,SETPUMP,0,""}; add_command(i); _smart_schedule.temp_reading_started_pump=true; }
    _smart_schedule.temp_reading_state=1; _smart_schedule.temp_reading_timer=_timestamp_secs+20;
}

void BWC::_processAccurateTempReading()
{
    if(!cio) return;
    if(_smart_schedule.temp_reading_state==1) {
        if(_timestamp_secs < _smart_schedule.temp_reading_timer) return;
        uint8_t t=cio->cio_states.temperature; if(!cio->cio_states.unit) t=(uint8_t)round(F2C(t));
        _smart_schedule.accurate_temperature=t; _smart_schedule.temp_reading_state=2; return;
    }
    if(_smart_schedule.temp_reading_state!=2) return;
    if(_smart_schedule.temp_reading_started_pump && cio->cio_states.pump && !cio->cio_states.heat) { command_que_item i{0,0,SETPUMP,0,""}; add_command(i); }
    const float hours=_calculateHeatingTime(_smart_schedule.accurate_temperature,_smart_schedule.target_temp);
    _smart_schedule.last_heating_estimate=hours;
    float buffer=hours*0.10f; if(buffer<1.0f) buffer=1.0f;
    uint64_t needed=(uint64_t)((hours+buffer)*3600.0f);
    uint64_t remaining=_smart_schedule.target_time>_timestamp_secs?_smart_schedule.target_time-_timestamp_secs:0;
    if(needed>=remaining) { _smart_schedule.calculated_start_time=_timestamp_secs; _smart_schedule.check_completed=true; }
    else {
        _smart_schedule.calculated_start_time=_smart_schedule.target_time-needed;
        uint64_t until=_smart_schedule.calculated_start_time-_timestamp_secs;
        uint64_t interval=until>86400ULL?43200ULL:until/2ULL; if(interval<300ULL) interval=300ULL;
        uint64_t proposed=_timestamp_secs+interval;
        if(proposed>=_smart_schedule.calculated_start_time) { _smart_schedule.check_completed=true; _smart_schedule.next_check_time=_smart_schedule.calculated_start_time; }
        else { _smart_schedule.check_completed=false; _smart_schedule.next_check_time=proposed; }
    }
    _smart_schedule.temp_reading_state=0; _smart_schedule.temp_reading_started_pump=false;
    _save_smartschedule_needed=true; _new_data_available=true;
}

void BWC::_handleSmartSchedulePanelOverride()
{
    if(!_smart_schedule.active || dsp == nullptr) return;

    const Buttons button = dsp->dsp_toggles.pressed_button;

    // Ein echter Tastendruck am Pumpenpanel hat immer Vorrang vor Smart
    // Schedule. HEAT beendet den Zeitplan direkt. PUMP und POWER beenden ihn
    // ebenfalls, sobald der Heizbetrieb bereits begonnen hat bzw. unmittelbar
    // bevorsteht. Dadurch kann Smart Schedule den manuellen AUS-Wunsch nicht
    // im selben oder naechsten Loop wieder mit SETHEATER=1 ueberschreiben.
    const bool heatOverride = (button == HEAT) || dsp->dsp_toggles.heat_change;
    const bool pumpOverride = (button == PUMP) || dsp->dsp_toggles.pump_change;
    const bool powerOverride = (button == POWER) || dsp->dsp_toggles.power_change;
    const bool heatingPhase = _smart_schedule.heater_started_by_schedule ||
                              (_smart_schedule.calculated_start_time != 0 &&
                               _timestamp_secs >= _smart_schedule.calculated_start_time);

    if(!heatOverride && !(heatingPhase && (pumpOverride || powerOverride))) return;

    // Bereits wartende, sofortige Smart-Schedule-/Restore-EIN-Befehle entfernen.
    // Ohne diese Bereinigung koennte ein kurz zuvor eingereihter SETHEATER=1
    // den physischen Tastendruck direkt wieder rueckgaengig machen.
    _command_que.erase(std::remove_if(_command_que.begin(), _command_que.end(),
        [this](const command_que_item& item){
            const bool immediateInternal = (item.interval == 0 && item.xtime == 0 && item.text.length() == 0);
            if(!immediateInternal) return false;
            if(item.cmd == SETHEATER && item.val == 1) return true;
            if(item.cmd == SETTARGET && item.val == _smart_schedule.target_temp) return true;
            if(item.cmd == SETPUMP && item.val == 1) return true;
            return false;
        }),
        _command_que.end());
    _save_cmdq_needed = true;

    // Nur den Smart-Schedule-Zustand verwerfen. Kein zusaetzlicher AUS-Befehl:
    // Der aktuelle physische Tastendruck wird direkt danach unveraendert an die
    // Pumpenelektronik weitergereicht. Ein zweiter Toggle waere hier falsch.
    _resetSmartScheduleState();
}

void BWC::_handleSmartSchedule()
{
    if(!_smart_schedule.active || !cio) return;
    if(_timestamp_secs >= _smart_schedule.target_time) {
        if(!_smart_schedule.keep_heater_on && _smart_schedule.heater_started_by_schedule && cio->cio_states.heat) { command_que_item i{0,0,SETHEATER,0,""}; add_command(i); }
        _resetSmartScheduleState(); return;
    }
    uint8_t temp=cio->cio_states.temperature; if(!cio->cio_states.unit) temp=(uint8_t)round(F2C(temp));
    if(!_smart_schedule.keep_heater_on && _smart_schedule.heater_started_by_schedule && temp>=_smart_schedule.target_temp) {
        if(cio->cio_states.heat) { command_que_item i{0,0,SETHEATER,0,""}; add_command(i); }
        _smart_schedule.target_temp_reached=true; _save_smartschedule_needed=true; return;
    }
    if(_smart_schedule.temp_reading_state) { _processAccurateTempReading(); return; }
    if(_smart_schedule.calculated_start_time && _timestamp_secs>=_smart_schedule.calculated_start_time) {
        if(!cio->cio_states.heat && !_smart_schedule.target_temp_reached) {
            command_que_item t{_smart_schedule.target_temp,0,SETTARGET,0,""}; add_command(t);
            command_que_item h{1,0,SETHEATER,0,""}; add_command(h);
            _smart_schedule.heater_started_by_schedule=true; _smart_schedule.check_completed=true; _save_smartschedule_needed=true;
        }
        return;
    }
    if(!_smart_schedule.check_completed && _timestamp_secs>=_smart_schedule.next_check_time) _startAccurateTempReading();
}

void BWC::getJSONSmartSchedule(String &rtn)
{
    StaticJsonDocument<1024> d; d[F("CONTENT")]=F("SMARTSCHEDULE"); d[F("ACTIVE")]=_smart_schedule.active; d[F("TARGETTIME")]=_smart_schedule.target_time; d[F("TARGETTEMP")]=_smart_schedule.target_temp; d[F("KEEPON")]=_smart_schedule.keep_heater_on; d[F("STARTTIME")]=_smart_schedule.calculated_start_time; d[F("NEXTCHECK")]=_smart_schedule.next_check_time; d[F("ESTIMATE")]=_smart_schedule.last_heating_estimate;
    float b=0; if(_smart_schedule.last_heating_estimate>0 && _smart_schedule.last_heating_estimate<999){b=_smart_schedule.last_heating_estimate*.10f;if(b<1)b=1;} d[F("BUFFER")]=b; d[F("ESTIMATED_KWH")]=_smart_schedule.last_heating_estimate>0?_smart_schedule.last_heating_estimate*2.0f:0; d[F("ESTIMATED_COST")]=(_smart_schedule.last_heating_estimate>0?_smart_schedule.last_heating_estimate*2.0f*(float)_price:0); d[F("CHECKCOMPLETED")]=_smart_schedule.check_completed; d[F("CURRENTTIME")]=_timestamp_secs;
    uint8_t temp=cio?cio->cio_states.temperature:0, target=cio?cio->cio_states.target:0; if(cio && !cio->cio_states.unit){temp=(uint8_t)round(F2C(temp));target=(uint8_t)round(F2C(target));} d[F("CURRENTTEMP")]=temp; d[F("GLOBALTARGET")]=target; d[F("ACCURATETEMP")]=_smart_schedule.accurate_temperature?_smart_schedule.accurate_temperature:temp; d[F("HEATER")]=cio?cio->cio_states.heat:0; d[F("PUMP")]=cio?cio->cio_states.pump:0; d[F("READING_STATE")]=_smart_schedule.temp_reading_state; d[F("POOLCAP")]=_pool_capacity; d[F("AMBIENTTEMP")]=_ambient_temp; d[F("TIMEREMAINING")]=(int64_t)_smart_schedule.target_time-(int64_t)_timestamp_secs; d[F("TIMEUNTILSTART")]=(int64_t)_smart_schedule.calculated_start_time-(int64_t)_timestamp_secs;
    float rem=-1; if(cio && cio->cio_states.heat) rem=temp>=_smart_schedule.target_temp?0:_calculateHeatingTime(temp,_smart_schedule.target_temp); d[F("REMAINING_HEATING_TIME")]=rem;
    char f[16]=""; if(_smart_schedule.last_heating_estimate>0&&_smart_schedule.last_heating_estimate<999){int m=(int)(_smart_schedule.last_heating_estimate*60);snprintf(f,sizeof(f),"%02d:%02d",m/60,m%60);} d[F("ESTIMATE_FMT")]=f; serializeJson(d,rtn);
}

void BWC::_loadSmartSchedule()
{
    File f=LittleFS.open(F("/smartschedule.json"),"r"); if(!f)return; StaticJsonDocument<512>d; if(deserializeJson(d,f)){f.close();return;} f.close();
    _smart_schedule.active=d[F("ACTIVE")]|false; _smart_schedule.target_time=d[F("TARGETTIME")]|0ULL; _smart_schedule.target_temp=d[F("TARGETTEMP")]|37; _smart_schedule.keep_heater_on=d[F("KEEPON")]|false; _smart_schedule.calculated_start_time=d[F("STARTTIME")]|0ULL; _smart_schedule.next_check_time=d[F("NEXTCHECK")]|0ULL; _smart_schedule.last_heating_estimate=d[F("ESTIMATE")]|0.0f; _smart_schedule.accurate_temperature=d[F("ACCURATETEMP")]|0; _smart_schedule.check_completed=d[F("CHECKCOMPLETED")]|false; _smart_schedule.heater_started_by_schedule=d[F("HEATEROWNED")]|false;
    _smart_schedule.temp_reading_state=0; _smart_schedule.temp_reading_started_pump=false; if(_smart_schedule.active && _smart_schedule.target_time<=(uint64_t)time(nullptr)) _smart_schedule.active=false;
}

void BWC::_saveSmartSchedule()
{
    _save_smartschedule_needed=false; File f=LittleFS.open(F("/smartschedule.json"),"w"); if(!f)return; StaticJsonDocument<512>d; d[F("ACTIVE")]=_smart_schedule.active; d[F("TARGETTIME")]=_smart_schedule.target_time; d[F("TARGETTEMP")]=_smart_schedule.target_temp; d[F("KEEPON")]=_smart_schedule.keep_heater_on; d[F("STARTTIME")]=_smart_schedule.calculated_start_time; d[F("NEXTCHECK")]=_smart_schedule.next_check_time; d[F("ESTIMATE")]=_smart_schedule.last_heating_estimate; d[F("ACCURATETEMP")]=_smart_schedule.accurate_temperature; d[F("CHECKCOMPLETED")]=_smart_schedule.check_completed; d[F("HEATEROWNED")]=_smart_schedule.heater_started_by_schedule; serializeJson(d,f); f.close();
}
