#include "CIO_6W.h"
#include "util.h"

void CIO_6W::handleToggles()
{
    sButton_queue_item item;
    _handleButtonQ();
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
        
        
        item.p_state = &sStates::char1;
        item.value = 0xFF;
        item.duration_ms = 100;
        _qButton(item);
        return;
    }

    
    if(cio_toggles.unit_change)
    {
        unlock();
        item.btncode = getButtonCode(UNIT);
        item.p_state = &sStates::unit;
        item.value = !cio_states.unit;
        item.duration_ms = 5000;
        _qButton(item);

        
        
        
        
        
        
        
        
        
        
        
    }

    
    if(cio_toggles.bubbles_change && getHasair())
    {
        unlock();
        item.btncode = getButtonCode(BUBBLES);
        item.p_state = &sStates::bubbles;
        item.value = !cio_states.bubbles;
        item.duration_ms = 5000;
        _qButton(item);
    }

    
    if(cio_toggles.heat_change)
    {
        unlock();
        item.btncode = getButtonCode(HEAT);
        item.p_state = &sStates::heat;
        item.value = !cio_states.heat;
        item.duration_ms = 5000;
        _qButton(item);
    }

    
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

    if((cio_toggles.target != cio_states.target) && (_button_que_len == 0))
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
        item.p_state = &sStates::char1; 
        item.value = 0xFF;              
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
    }

    if((cio_toggles.down_pressed) && (_button_que_len == 0))
    {
        unlock();
        item.btncode = getButtonCode(DOWN);
        item.p_state = &sStates::char1;
        item.value = 0xFF;
        item.duration_ms = 100;
        _qButton(item);
    }

    
}

void CIO_6W::unlock()
{
    sButton_queue_item item;
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
}

void CIO_6W::_qButton(sButton_queue_item item) {
    if(_button_que_len >= MAXBUTTONS) return;  
    _button_que[_button_que_len].btncode = item.btncode;
    _button_que[_button_que_len].p_state = item.p_state;
    _button_que[_button_que_len].value = item.value;
    _button_que[_button_que_len].duration_ms = item.duration_ms;
    _button_que_len++;
    
}

void CIO_6W::_handleButtonQ(void) {
    static uint32_t prevMillis = millis();
    static uint32_t elapsedTime = 0;

    elapsedTime = millis() - prevMillis;
    prevMillis = millis();
    uint8_t waitlimit = 0;
    if(_button_que_len == 0)
    
    
    
    
    
    
    
    
    
        return;
    
    
    _button_que[0].duration_ms -= elapsedTime;
    
    if( (cio_states.*_button_que[0].p_state == _button_que[0].value) || (_button_que[0].duration_ms <= 0) )
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
    }
    else
    {
        
        
        waitlimit = 0;
        while(_packet_transm_active && ++waitlimit < 10) delay(1);
        _button_code = _button_que[0].btncode;
    }
}

CIO_6W::CIO_6W()
{
    cio_states.target = 0;
}
