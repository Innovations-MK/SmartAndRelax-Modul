#include "CIO_6W.h"
#include "util.h"

void CIO_6W::handleToggles()
{
    sButton_queue_item item;
    _handleButtonQ();

                                                                              
                                                                               
                                                                              
                                                             
    const bool physicalTargetInput = cio_toggles.up_pressed || cio_toggles.down_pressed;
    if(physicalTargetInput) _preemptAutomaticTargetQueueForPhysicalInput();

    if(_button_que_len > 30) return;
                                                                                         
                                                                                                                          
    if(cio_toggles.power_change)
    {
        item.btncode = getButtonCode(POWER);
        item.p_state = &sStates::power;
        item.value = !cio_states.power;
        item.duration_ms = 5000;
        _qButton(item);
        return;
    }

                                                                            
                                                                               
                                                                      
                                                                               
                                                           
    if(cio_toggles.lock_change && (_button_que_len == 0))
    {
        item.btncode = getButtonCode(LOCK);
        item.p_state = &sStates::locked;
        item.value = !cio_states.locked;
        item.duration_ms = 5000;
        _qButton(item);
        return;
    }

    if(cio_toggles.locked_pressed && (_button_que_len == 0))
    {
        item.btncode = getButtonCode(LOCK);
        // Physical button passthrough: the DSP keeps this flag asserted while
        // the user holds LOCK, therefore 100 ms chunks are intentional here.
        item.p_state = &sStates::char1;
        item.value = 0xFF;
        item.duration_ms = 100;
        _qButton(item);
        return;
    }

    // if(requestedStates.unit != _requested_states.unit)
    if(cio_toggles.unit_change)
    {
        unlock();
        item.btncode = getButtonCode(UNIT);
        item.p_state = &sStates::unit;
        item.value = !cio_states.unit;
        item.duration_ms = 5000;
        _qButton(item);

        // if((requestedStates.target >= 20) && (requestedStates.target <= 104) && (requestedStates.target == _requested_states.target))
        // {
        //     if(requestedStates.unit)
        //     {
        //         cio_states.target = round(F2C(requestedStates.target));
        //     }
        //     else
        //     {
        //         cio_states.target = round(C2F(requestedStates.target));
        //     }
        // }
    }

    // if((requestedStates.bubbles != _requested_states.bubbles) && getHasair())
    if(cio_toggles.bubbles_change && getHasair())
    {
        unlock();
        item.btncode = getButtonCode(BUBBLES);
        item.p_state = &sStates::bubbles;
        item.value = !cio_states.bubbles;
        item.duration_ms = 5000;
        _qButton(item);
    }

    // if(requestedStates.heat != _requested_states.heat)
    if(cio_toggles.heat_change)
    {
        unlock();
        item.btncode = getButtonCode(HEAT);
        item.p_state = &sStates::heat;
        item.value = !cio_states.heat;
        item.duration_ms = 5000;
        _qButton(item);
    }

    // if(requestedStates.pump != _requested_states.pump)
    if(cio_toggles.pump_change)
    {
        unlock();
        item.btncode = getButtonCode(PUMP);
        item.p_state = &sStates::pump;
        item.value = !cio_states.pump;
        item.duration_ms = 5000;
        _qButton(item);
    }

    if(cio_states.target == 0 && _button_que_len == 0)
    {
        unlock();
        item.btncode = getButtonCode(UP);
        item.p_state = &sStates::target;
        item.value = 999;
        item.duration_ms = 700;
        _qButton(item);
        item.btncode = getButtonCode(NOBTN);
        item.p_state = &sStates::char1;
        item.value = 0xFF;
        item.duration_ms = 500;
        _qButton(item);
    }

    // Physical panel input has priority. Do not start/restart an automatic
    // SETTARGET UP/DOWN sequence in the same loop in which the user presses
    // UP or DOWN on the pump itself.
    if(!physicalTargetInput && (cio_toggles.target != cio_states.target) && (_button_que_len == 0))
    {
        unlock();
        Buttons dir;
        cio_toggles.target > cio_states.target ? dir = UP : dir = DOWN;
        item.btncode = getButtonCode(dir);
        item.p_state = &sStates::target;
        item.value = cio_toggles.target;
        item.duration_ms = 800;
        _qButton(item);
        item.btncode = getButtonCode(NOBTN);
        item.p_state = &sStates::char1;
        item.value = 0xFF;
        item.duration_ms = 400;
        _qButton(item);
    }

    // if((requestedStates.jets != _requested_states.jets) && getHasjets())
    if(cio_toggles.jets_change && getHasjets())
    {
        unlock();
        item.btncode = getButtonCode(HYDROJETS);
        item.p_state = &sStates::jets;
        item.value = !cio_states.jets;
        item.duration_ms = 5000;
        _qButton(item);
    }

    if((cio_toggles.timer_pressed) && (_button_que_len == 0))
    {
        unlock();
        item.btncode = getButtonCode(TIMER);
        item.p_state = &sStates::char1; //This field will never meet the value condition.
        item.value = 0xFF;              //Just a trick to press this button for the duration, no matter what
        item.duration_ms = 100;
        _qButton(item);
    }

    if((cio_toggles.up_pressed) && (_button_que_len == 0))
    {
        unlock();
        item.btncode = getButtonCode(UP);
        item.p_state = &sStates::char1;
        item.value = 0xFF;
        item.duration_ms = 100;
        _qButton(item);
        // TouchFast1: do not wait for the next BWC loop before presenting a
        // freshly detected physical temperature button to the CIO.
        _handleButtonQ();
        physical_target_immediate_start_count++;
        return;
    }

    if((cio_toggles.down_pressed) && (_button_que_len == 0))
    {
        unlock();
        item.btncode = getButtonCode(DOWN);
        item.p_state = &sStates::char1;
        item.value = 0xFF;
        item.duration_ms = 100;
        _qButton(item);
        // Same-loop start for physical DOWN. This removes one complete main
        // loop of avoidable latency while retaining the existing 100 ms pulse.
        _handleButtonQ();
        physical_target_immediate_start_count++;
        return;
    }

    // _pressed_button = cio_toggles.pressed_button; TODO: remove variable from class
}

void CIO_6W::unlock()
{
    sButton_queue_item item;
                                                                           
                                                                         
                                                                              
                                                                             
    if(!cio_states.power)
    {
        item.btncode = getButtonCode(POWER);
        item.p_state = &sStates::power;
        item.value = 1;
        item.duration_ms = 5000;
        _qButton(item);

                                                                         
                                                                            
                                                                          
        item.btncode = getButtonCode(LOCK);
        item.p_state = &sStates::locked;
        item.value = 0;
        item.duration_ms = 5000;
        _qButton(item);
        return;
    }
    if(cio_states.locked)
    {
        item.btncode = getButtonCode(LOCK);
        item.p_state = &sStates::locked;
        item.value = 0;
        item.duration_ms = 5000;
        _qButton(item);
    }
}


bool CIO_6W::_preemptAutomaticTargetQueueForPhysicalInput()
{
    if(_button_que_len == 0) return false;

    const uint16_t noButtonCode = getButtonCode(NOBTN);
    const bool activeTargetStep = (_button_que[0].p_state == &sStates::target);
    // In CIO_6W the only queued NOBTN items are the release gaps appended to
    // automatic target-temperature sequences. If such a gap is still active,
    // a physical UP/DOWN press should not have to wait another 400/500 ms.
    const bool activeTargetRelease = (_button_que[0].btncode == noButtonCode);

    if(!activeTargetStep && !activeTargetRelease) return false;

    _button_que_len = 0;
    physical_target_preempt_count++;

    uint8_t waitlimit = 0;
    while(_packet_transm_active && ++waitlimit < 10) delay(1);
    _button_code = noButtonCode;
    return true;
}

void CIO_6W::_noteTargetButtonActivity(bool new_press)
{
    target_capture_last_button_ms = millis();
    if(new_press) target_capture_arm_count++;
}

uint32_t CIO_6W::_targetCaptureAgeMs() const
{
    if(target_capture_last_button_ms == 0) return 0xFFFFFFFFUL;
    return (uint32_t)(millis() - target_capture_last_button_ms);
}

int CIO_6W::_parseDisplayNumber(char c1, char c2, char c3) const
{
    const char chars[3] = {c1, c2, c3};
    int value = 0;
    bool haveDigit = false;
    for(uint8_t i = 0; i < 3; i++)
    {
        const char c = chars[i];
        if(c >= '0' && c <= '9')
        {
            haveDigit = true;
            value = value * 10 + (c - '0');
            continue;
        }
        if(c == ' ') continue;
        return -1;
    }
    return haveDigit ? value : -1;
}

bool CIO_6W::_acceptTargetValue(int value)
{
    // Preserve the original lower bound (>19) and add only a generous upper
    // sanity limit so segment glitches such as 888 can never become a target.
    // This covers both Celsius (20..40) and Fahrenheit (68..104) operation.
    if(value <= 19 || value > 110) return false;
    cio_states.target = (uint8_t)value;
    target_capture_last_value = (uint8_t)value;
    target_capture_last_value_ms = millis();
    target_capture_accept_count++;
    return true;
}

void CIO_6W::_qButton(sButton_queue_item item) {
    if(_button_que_len >= MAXBUTTONS) return;                                                    
    _button_que[_button_que_len].btncode = item.btncode;
    _button_que[_button_que_len].p_state = item.p_state;
    _button_que[_button_que_len].value = item.value;
                                                                          
                                                                        
    _button_que[_button_que_len].duration_ms = -abs(item.duration_ms);
    _button_que_len++;
    /*"_button_que[0] = item" may not copy values but pointer to item which goes out of scope. Not sure.*/
}

void CIO_6W::_handleButtonQ(void) {
    static uint32_t prevMillis = millis();
    static uint32_t elapsedTime = 0;

    elapsedTime = millis() - prevMillis;
    prevMillis = millis();
    uint8_t waitlimit = 0;
    if(_button_que_len == 0)
    // {
    //     /*Buttonqueue is empty, so let the touchbuttons from display/bwc through*/
    //     /*Avoiding write to variable when it's beeing sent to the cio*/
    //     waitlimit = 0;
    //     while(_packet_transm_active && ++waitlimit < 10) delay(1);
    //     _button_code = getButtonCode(NOBTN);
    //     if(_pressed_button == TIMER) _button_code = getButtonCode(TIMER);
    //     if(_pressed_button == UP) _button_code = getButtonCode(UP);
    //     if(_pressed_button == DOWN) _button_code = getButtonCode(DOWN);
        return;
    // }

                                                                              
                                                                             
                                                                              
                                                                             
                                                                            
    if(_button_que[0].duration_ms < 0)
    {
        // If the requested state is already true (e.g. an unlock prerequisite
        // that became satisfied meanwhile), discard it without pressing.
        if(cio_states.*_button_que[0].p_state == _button_que[0].value)
        {
            for(int i = 0; i < _button_que_len-1; i++){
                _button_que[i].btncode = _button_que[i+1].btncode;
                _button_que[i].p_state = _button_que[i+1].p_state;
                _button_que[i].value = _button_que[i+1].value;
                _button_que[i].duration_ms = _button_que[i+1].duration_ms;
            }
            _button_que_len--;
            waitlimit = 0;
            while(_packet_transm_active && ++waitlimit < 10) delay(1);
            _button_code = getButtonCode(NOBTN);
            return;
        }

        waitlimit = 0;
        while(_packet_transm_active && ++waitlimit < 10) delay(1);
        _button_code = _button_que[0].btncode;
        if((_button_code == getButtonCode(UP)) || (_button_code == getButtonCode(DOWN)))
            _noteTargetButtonActivity(true);
        _button_que[0].duration_ms = -_button_que[0].duration_ms;
        // Full duration starts now, not when the item was enqueued.
        prevMillis = millis();
        return;
    }

    // First subtract elapsed time from maxduration
    _button_que[0].duration_ms -= elapsedTime;
    //check if state is as desired, or duration is up. If so - remove row. Else set BTNCODE
    if( (cio_states.*_button_que[0].p_state == _button_que[0].value) || (_button_que[0].duration_ms <= 0) )
    {
        //remove row
        for(int i = 0; i < _button_que_len-1; i++){
            _button_que[i].btncode = _button_que[i+1].btncode;
            _button_que[i].p_state = _button_que[i+1].p_state;
            _button_que[i].value = _button_que[i+1].value;
            _button_que[i].duration_ms = _button_que[i+1].duration_ms;
        }
        _button_que_len--;
        /*Avoiding write to variable when it's beeing sent to the cio*/
        waitlimit = 0;
        while(_packet_transm_active && ++waitlimit < 10) delay(1);
        _button_code = getButtonCode(NOBTN);
    }
    else
    {
        //keep button "pressed"
        /*Avoiding write to variable when it's beeing sent to the cio*/
        waitlimit = 0;
        while(_packet_transm_active && ++waitlimit < 10) delay(1);
        _button_code = _button_que[0].btncode;
        if((_button_code == getButtonCode(UP)) || (_button_code == getButtonCode(DOWN)))
            _noteTargetButtonActivity(false);
    }
}

CIO_6W::CIO_6W()
{
    cio_states.target = 0;
}
