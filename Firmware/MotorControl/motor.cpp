
#include "motor.hpp"
#include "axis.hpp"
#include "low_level.h"
#include "odrive_main.h"

#include <algorithm>

#define CURRENT_ADC_LOWER_BOUND         (uint32_t)((float)(1 << 12) * CURRENT_SENSE_MIN_VOLT / 3.3f)
#define CURRENT_ADC_UPPER_BOUND         (uint32_t)((float)(1 << 12) * CURRENT_SENSE_MAX_VOLT / 3.3f)

/**
 * @brief This control law adjusts the output voltage such that a predefined
 * current is tracked. A hardcoded integrator gain is used for this.
 * 
 * TODO: this might as well be implemented using the FieldOrientedController.
 */
struct ResistanceMeasurementCotnrolLaw : AlphaBetaFrameController {
    void reset() final {
        test_voltage_ = 0.0f;
        test_mod_ = NAN;
    }

    ODriveIntf::MotorIntf::Error on_measurement(
        float vbus_voltage, float Ialpha, float Ibeta,
        uint32_t input_timestamp) final
    {
        test_voltage_ += (kI * current_meas_period) * (test_current_ - Ialpha);
    
        if (std::abs(test_voltage_) > max_voltage_) {
            test_voltage_ = NAN;
            return Motor::ERROR_PHASE_RESISTANCE_OUT_OF_RANGE;
        } else if (std::isnan(vbus_voltage)) {
            return Motor::ERROR_UNKNOWN_VBUS_VOLTAGE;
        } else {
            float vfactor = 1.0f / ((2.0f / 3.0f) * vbus_voltage);
            test_mod_ = test_voltage_ * vfactor;
            return Motor::ERROR_NONE;
        }
    }

    std::variant<std::tuple<float, float>, ODriveIntf::MotorIntf::Error> get_alpha_beta_output(uint32_t output_timestamp) {
        if (std::isnan(test_mod_)) {
            return {Motor::ERROR_CONTROLLER_INITIALIZING};
        } else {
            return std::make_tuple(test_mod_, 0.0f);
        }
    }

    float get_resistance() {
        return test_voltage_ / test_current_;
    }

    const float kI = 10.0f; // [(V/s)/A]
    float max_voltage_ = 0.0f;
    float test_current_ = 0.0f;
    float test_voltage_ = 0.0f;
    float test_mod_ = NAN;
};

/**
 * @brief This control law toggles rapidly between positive and negative output
 * voltage. By measuring how large the current ripples are, the phase inductance
 * can be determined.
 * 
 * TODO: this method assumes a certain synchronization between current measurement and output application
 */
struct InductanceMeasurementCotnrolLaw : AlphaBetaFrameController {
    void reset() final {
        attached_ = false;
    }

    ODriveIntf::MotorIntf::Error on_measurement(float vbus_voltage,
        float Ialpha, float Ibeta, uint32_t input_timestamp) final
    {
        if (std::isnan(Ialpha) || std::isnan(vbus_voltage)) {
            return {Motor::ERROR_UNKNOWN_VBUS_VOLTAGE};
        }

        if (attached_) {
            float sign = test_voltage_ >= 0.0f ? 1.0f : -1.0f;
            deltaI_ += -sign * (Ialpha - last_Ialpha_);
        } else {
            start_timestamp_ = input_timestamp;
            attached_ = true;
        }

        last_Ialpha_ = Ialpha;
        last_input_timestamp_ = input_timestamp;

        return Motor::ERROR_NONE;
    }

    std::variant<std::tuple<float, float>, ODriveIntf::MotorIntf::Error> get_alpha_beta_output(
        uint32_t output_timestamp) final
    {
        test_voltage_ *= -1.0f;
        float vfactor = 1.0f / ((2.0f / 3.0f) * vbus_voltage);
        return std::make_tuple(test_voltage_ * vfactor, 0.0f);
    }

    float get_inductance() {
        // Note: A more correct formula would also take into account that there is a finite timestep.
        // However, the discretisation in the current control loop inverts the same discrepancy
        float dt = (float)(last_input_timestamp_ - start_timestamp_) / (float)TIM_1_8_CLOCK_HZ; // at 216MHz this overflows after 19 seconds
        return std::abs(test_voltage_) / (deltaI_ / dt);
    }

    // Config
    float test_voltage_ = 0.0f;

    // State
    bool attached_ = false;
    float sign_ = 0;

    // Outputs
    uint32_t start_timestamp_ = 0;
    float last_Ialpha_ = NAN;
    uint32_t last_input_timestamp_ = 0;
    float deltaI_ = 0.0f;
};


Motor::Motor(TIM_HandleTypeDef* timer,
             uint8_t current_sensor_mask,
             float shunt_conductance,
             TGateDriver& gate_driver,
             TOpAmp& opamp) :
        timer_(timer),
        current_sensor_mask_(current_sensor_mask),
        shunt_conductance_(shunt_conductance),
        gate_driver_(gate_driver),
        opamp_(opamp) {
    apply_config();
}

/**
 * @brief Arms the PWM outputs that belong to this motor.
 *
 * Note that this does not activate the PWM outputs immediately, it just sets
 * a flag so they will be enabled later.
 * 
 * The sequence goes like this:
 *  - Motor::arm() sets the is_armed_ flag.
 *  - On the next timer update event Motor::timer_update_cb() gets called in an
 *    interrupt context
 *  - Motor::timer_update_cb() runs specified control law to determine PWM values
 *  - Motor::timer_update_cb() calls Motor::apply_pwm_timings()
 *  - Motor::apply_pwm_timings() sets the output compare registers and the AOE
 *    (automatic output enable) bit.
 *  - On the next update event the timer latches the configured values into the
 *    active shadow register and enables the outputs at the same time.
 * 
 * The sequence can be aborted at any time by calling Motor::disarm().
 *
 * @param control_law: An control law that is called at the frequency of current
 *        measurements. The function must return as quickly as possible
 *        such that the resulting PWM timings are available before the next
 *        timer update event.
 * @returns: True on success, false otherwise
 */
bool Motor::arm(PhaseControlLaw<3>* control_law) {
    CRITICAL_SECTION() {
        control_law_ = control_law;

        // Reset controller states, integrators, setpoints, etc.
        axis_->controller_.reset();
        axis_->async_estimator_.rotor_flux_ = 0.0f;
        if (control_law_) {
            control_law_->reset();
        }

        if (brake_resistor_armed) {
            is_armed_ = true;
        }
    }

    return true;
}

/**
 * @brief Updates the phase PWM timings unless the motor is disarmed.
 *
 * If the motor is armed, the PWM timings come into effect at the next update
 * event (and are enabled if they weren't already), unless the motor is disarmed
 * prior to that.
 * 
 * @param tentative: If true, the update is not counted as "refresh".
 */
void Motor::apply_pwm_timings(uint16_t timings[3], bool tentative) {
    CRITICAL_SECTION() {
        if (!brake_resistor_armed) {
            disarm_with_error(ERROR_BRAKE_RESISTOR_DISARMED);
        }

        TIM_HandleTypeDef* htim = timer_;
        TIM_TypeDef* tim = htim->Instance;
        tim->CCR1 = timings[0];
        tim->CCR2 = timings[1];
        tim->CCR3 = timings[2];
        
        if (!tentative) {
            if (is_armed_) {
                // Set the Automatic Output Enable so that the Master Output Enable
                // bit will be automatically enabled on the next update event.
                tim->BDTR |= TIM_BDTR_AOE;
            }
        }
        
        // If a timer update event occurred just now while we were updating the
        // timings, we can't be sure what values the shadow registers now contain,
        // so we must disarm the motor.
        // (this also protects against the case where the update interrupt has too
        // low priority, but that should not happen)
        if (__HAL_TIM_GET_FLAG(htim, TIM_FLAG_UPDATE)) {
            disarm_with_error(ERROR_CONTROL_DEADLINE_MISSED);
        }
    }
}

/**
 * @brief Disarms the motor PWM.
 * 
 * After this function returns, it is guaranteed that all three
 * motor phases are floating and will not be enabled again until
 * arm() is called.
 */
bool Motor::disarm(bool* was_armed) {
    bool dummy;
    was_armed = was_armed ? was_armed : &dummy;
    CRITICAL_SECTION() {
        *was_armed = is_armed_;
        if (is_armed_) {
            gate_driver_.set_enabled(false);
        }
        is_armed_ = false;
        TIM_HandleTypeDef* timer = timer_;
        timer->Instance->BDTR &= ~TIM_BDTR_AOE; // prevent the PWMs from automatically enabling at the next update
        __HAL_TIM_MOE_DISABLE_UNCONDITIONALLY(timer);
        control_law_ = nullptr;
    }

    // Check necessary to prevent infinite recursion
    if (was_armed) {
        update_brake_current();
    }

    return true;
}

// @brief Tune the current controller based on phase resistance and inductance
// This should be invoked whenever one of these values changes.
// TODO: allow update on user-request or update automatically via hooks
void Motor::update_current_controller_gains() {
    // Calculate current control gains
    current_control_.p_gain_ = config_.current_control_bandwidth * config_.phase_inductance;
    float plant_pole = config_.phase_resistance / config_.phase_inductance;
    current_control_.i_gain_ = plant_pole * current_control_.p_gain_;
}

bool Motor::apply_config() {
    config_.parent = this;
    is_calibrated_ = config_.pre_calibrated;
    update_current_controller_gains();
    return true;
}

// @brief Set up the gate drivers
bool Motor::setup() {
    // Solve for exact gain, then snap down to have equal or larger range as requested
    // or largest possible range otherwise
    constexpr float kMargin = 0.90f;
    constexpr float max_output_swing = 1.35f; // [V] out of amplifier
    float max_unity_gain_current = kMargin * max_output_swing * shunt_conductance_; // [A]
    float requested_gain = max_unity_gain_current / config_.requested_current_range; // [V/V]
    
    float actual_gain = NAN;
    if (!gate_driver_.config(requested_gain, &actual_gain))
        return false;

    // Values for current controller
    phase_current_rev_gain_ = 1.0f / actual_gain;
    // Clip all current control to actual usable range
    max_allowed_current_ = max_unity_gain_current * phase_current_rev_gain_;

    max_dc_calib_ = 0.1f * max_allowed_current_;

    if (!gate_driver_.init())
        return true;

    return true;
}

void Motor::disarm_with_error(Motor::Error error){
    error_ |= error;
    disarm();
    update_brake_current();
}

bool Motor::do_checks(uint32_t timestamp) {
    gate_driver_.do_checks();

    if (!gate_driver_.is_ready()) {
        disarm_with_error(ERROR_DRV_FAULT);
        return false;
    }

    return true;
}

float Motor::effective_current_lim() {
    // Configured limit
    float current_lim = config_.current_lim;
    // Hardware limit
    if (axis_->motor_.config_.motor_type == Motor::MOTOR_TYPE_GIMBAL) {
        current_lim = std::min(current_lim, 0.98f*one_by_sqrt3*vbus_voltage); //gimbal motor is voltage control
    } else {
        current_lim = std::min(current_lim, axis_->motor_.max_allowed_current_);
    }

    // Apply axis current limiters
    for (const CurrentLimiter* const limiter : axis_->current_limiters_) {
        current_lim = std::min(current_lim, limiter->get_current_limit(config_.current_lim));
    }

    effective_current_lim_ = current_lim;

    return effective_current_lim_;
}

//return the maximum available torque for the motor.
//Note - for ACIM motors, available torque is allowed to be 0.
float Motor::max_available_torque() {
    if (config_.motor_type == Motor::MOTOR_TYPE_ACIM) {
        float max_torque = effective_current_lim_ * config_.torque_constant * axis_->async_estimator_.rotor_flux_;
        max_torque = std::clamp(max_torque, 0.0f, config_.torque_lim);
        return max_torque;
    } else {
        float max_torque = effective_current_lim_ * config_.torque_constant;
        max_torque = std::clamp(max_torque, 0.0f, config_.torque_lim);
        return max_torque;
    }
}

void Motor::log_timing(TimingLog_t log_idx) {
    static const uint16_t clocks_per_cnt = (uint16_t)((float)TIM_1_8_CLOCK_HZ / (float)TIM_APB1_CLOCK_HZ);
    uint16_t timing = clocks_per_cnt * htim13.Instance->CNT; // TODO: Use a hw_config

    if (log_idx < TIMING_LOG_NUM_SLOTS) {
        timing_log_[log_idx] = timing;
    }
}

float Motor::phase_current_from_adcval(uint32_t ADCValue) {
    int adcval_bal = (int)ADCValue - (1 << 11);
    float amp_out_volt = (3.3f / (float)(1 << 12)) * (float)adcval_bal;
    float shunt_volt = amp_out_volt * phase_current_rev_gain_;
    float current = shunt_volt * shunt_conductance_;
    return current;
}

//--------------------------------
// Measurement and calibration
//--------------------------------

// TODO check Ibeta balance to verify good motor connection
bool Motor::measure_phase_resistance(float test_current, float max_voltage) {
    ResistanceMeasurementCotnrolLaw control_law;
    control_law.test_current_ = test_current;
    control_law.max_voltage_ = max_voltage;

    arm(&control_law);

    for (size_t i = 0; i < 3000; ++i) {
        if (!((axis_->requested_state_ == Axis::AXIS_STATE_UNDEFINED) && axis_->motor_.is_armed_)) {
            break;
        }
        osDelay(1);
    }

    bool success = is_armed_;

    //// De-energize motor
    //if (!enqueue_voltage_timings(motor, 0.0f, 0.0f))
    //    return false; // error set inside enqueue_voltage_timings

    disarm();

    config_.phase_resistance = control_law.get_resistance();
    if (std::isnan(config_.phase_resistance)) {
        // TODO: the motor is already disarmed at this stage. This is an error
        // that only pretains to the measurement and its result so it should
        // just be a return value of this function.
        disarm_with_error(ERROR_PHASE_RESISTANCE_OUT_OF_RANGE);
        success = false;
    }

    return success;
}


bool Motor::measure_phase_inductance(float test_voltage) {
    InductanceMeasurementCotnrolLaw control_law;
    control_law.test_voltage_ = test_voltage;

    arm(&control_law);

    for (size_t i = 0; i < 1250; ++i) {
        if (!((axis_->requested_state_ == Axis::AXIS_STATE_UNDEFINED) && axis_->motor_.is_armed_)) {
            break;
        }
        osDelay(1);
    }

    bool success = is_armed_;

    //// De-energize motor
    //if (!enqueue_voltage_timings(motor, 0.0f, 0.0f))
    //    return false; // error set inside enqueue_voltage_timings

    disarm();

    config_.phase_inductance = control_law.get_inductance();
    
    // TODO arbitrary values set for now
    if (!(config_.phase_inductance >= 2e-6f && config_.phase_inductance <= 4000e-6f)) {
        error_ |= ERROR_PHASE_INDUCTANCE_OUT_OF_RANGE;
        success = false;
    }

    return success;
}


// TODO: motor calibration should only be a utility function that's called from
// the UI on explicit user request. It should take its parameters as input
// arguments and return the measured results without modifying any config values.
bool Motor::run_calibration() {
    float R_calib_max_voltage = config_.resistance_calib_max_voltage;
    if (config_.motor_type == MOTOR_TYPE_HIGH_CURRENT
        || config_.motor_type == MOTOR_TYPE_ACIM) {
        if (!measure_phase_resistance(config_.calibration_current, R_calib_max_voltage))
            return false;
        if (!measure_phase_inductance(R_calib_max_voltage))
            return false;
    } else if (config_.motor_type == MOTOR_TYPE_GIMBAL) {
        // no calibration needed
    } else {
        return false;
    }

    update_current_controller_gains();
    
    is_calibrated_ = true;
    return true;
}


/**
 * @brief Called when the underlying hardware timer triggers an update event.
 */
void Motor::tim_update_cb(uint32_t adc_a, uint32_t adc_b, uint32_t adc_c) {
    last_update_timestamp_ += TIM_1_8_PERIOD_CLOCKS * (TIM_1_8_RCR + 1);

    // If the corresponding timer is counting up, we just sampled in SVM vector 0, i.e. real current
    // If we are counting down, we just sampled in SVM vector 7, with zero current
    bool counting_down = timer_->Instance->CR1 & TIM_CR1_DIR;

    bool timer_update_missed = (counting_down_ == counting_down);
    if (timer_update_missed) {
        disarm_with_error(ERROR_TIMER_UPDATE_MISSED);
        return;
    }
    counting_down_ = counting_down;

    log_timing(TIMING_LOG_UPDATE_START);

    // Decode which actions to run for this update event.
    const bool should_update_pwm = counting_down_;
    const bool was_current_dc_calib = counting_down_;
    const bool was_current_sense = !counting_down_;

    if (should_update_pwm) {
        // Tentatively reset PWM values to 50% duty cycle in case the
        // function does not succeed for any reason or misses the timing
        // deadline.
        uint16_t half_timings[] = {TIM_1_8_PERIOD_CLOCKS / 2, TIM_1_8_PERIOD_CLOCKS / 2, TIM_1_8_PERIOD_CLOCKS / 2};
        apply_pwm_timings(half_timings, true);
    }

    // Make sure the measurements don't come too close to the current sensor's hardware limitations
    if ((adc_a != 0xffffffff) && (adc_a < CURRENT_ADC_LOWER_BOUND || adc_a > CURRENT_ADC_UPPER_BOUND)) {
        disarm_with_error(ERROR_CURRENT_SENSE_SATURATION);
        adc_a = 0xffffffff;
    }
    if ((adc_b != 0xffffffff) && (adc_b < CURRENT_ADC_LOWER_BOUND || adc_b > CURRENT_ADC_UPPER_BOUND)) {
        disarm_with_error(ERROR_CURRENT_SENSE_SATURATION);
        adc_b = 0xffffffff;
    }
    if ((adc_c != 0xffffffff) && (adc_c < CURRENT_ADC_LOWER_BOUND || adc_c > CURRENT_ADC_UPPER_BOUND)) {
        disarm_with_error(ERROR_CURRENT_SENSE_SATURATION);
        adc_c = 0xffffffff;
    }

    // Convert ADC readings to current values
    Iph_ABC_t current = {
        (adc_a != 0xffffffff) ? phase_current_from_adcval(adc_a) : NAN,
        (adc_b != 0xffffffff) ? phase_current_from_adcval(adc_b) : NAN,
        (adc_c != 0xffffffff) ? phase_current_from_adcval(adc_c) : NAN
    };

    // Infer the missing current value from the other two (if applicable)
    switch (current_sensor_mask_) {
        case 0b110: current.phA = -(current.phB + current.phC); break;
        case 0b101: current.phB = -(current.phC + current.phA); break;
        case 0b011: current.phC = -(current.phA + current.phB); break;
        case 0b111: break;
    }

    bool current_valid = !std::isnan(current.phA)
                      && !std::isnan(current.phB)
                      && !std::isnan(current.phC);

    log_timing(TIMING_LOG_CURRENT_MEAS);

    const float interrupt_period = static_cast<float>(TIM_1_8_PERIOD_CLOCKS * (TIM_1_8_RCR + 1)) / TIM_1_8_CLOCK_HZ;

    if (was_current_dc_calib) {
        if (current_valid) {
            const float dc_calib_period = 2 * interrupt_period;
            const float calib_filter_k = std::min(dc_calib_period / config_.dc_calib_tau, 1.0f);
            DC_calib_.phA += (current.phA - DC_calib_.phA) * calib_filter_k;
            DC_calib_.phB += (current.phB - DC_calib_.phB) * calib_filter_k;
            DC_calib_.phC += (current.phC - DC_calib_.phC) * calib_filter_k;
            dc_calib_running_since_ += dc_calib_period;
        } else {
            DC_calib_.phA = 0.0f;
            DC_calib_.phB = 0.0f;
            DC_calib_.phC = 0.0f;
            dc_calib_running_since_ = 0.0f;
        }
        
        log_timing(TIMING_LOG_DC_CAL);
    }
    
    if (was_current_sense) {
        bool dc_calib_valid = (dc_calib_running_since_ >= config_.dc_calib_tau * 7.5f)
                           && (abs(DC_calib_.phA) < max_dc_calib_)
                           && (abs(DC_calib_.phB) < max_dc_calib_)
                           && (abs(DC_calib_.phC) < max_dc_calib_);

        if (current_valid && dc_calib_valid) {
            current.phA -= DC_calib_.phA;
            current.phB -= DC_calib_.phB;
            current.phC -= DC_calib_.phC;
            I_leak_ = current.phA + current.phB + current.phC; // sum should be close to 0
            current_meas_.phA = current.phA - I_leak_ / 3.0f;
            current_meas_.phB = current.phB - I_leak_ / 3.0f;
            current_meas_.phC = current.phC - I_leak_ / 3.0f;
        } else {
            I_leak_ = NAN;
            current_meas_.phA = NAN;
            current_meas_.phB = NAN;
            current_meas_.phC = NAN;
        }

        if (abs(I_leak_) > config_.I_leak_max) {
            disarm_with_error(ERROR_I_LEAK_OUT_OF_RANGE);
        }

        // Run system-level checks (e.g. overvoltage/undervoltage condition)
        // The motor might be disarmed in this function. In this case the
        // handler will continue to run until the end but it won't have an
        // effect on the PWM.
        odrv.do_fast_checks();

        // Check for violation of current limit
        // If Ia + Ib + Ic == 0 holds then we have:
        // Inorm^2 = Id^2 + Iq^2 = Ialpha^2 + Ibeta^2 = 2/3 * (Ia^2 + Ib^2 + Ic^2)
        float Itrip = effective_current_lim_ + config_.current_lim_margin;
        if (2.0f / 3.0f * (SQ(current_meas_.phA) + SQ(current_meas_.phB) + SQ(current_meas_.phC)) > SQ(Itrip)) {
            disarm_with_error(ERROR_CURRENT_LIMIT_VIOLATION);
        }

        if (control_law_) {
            Error err = control_law_->on_measurement(vbus_voltage,
                                {current_meas_.phA, current_meas_.phB, current_meas_.phC},
                                last_update_timestamp_);
            if (err != ERROR_NONE) {
                disarm_with_error(err);
            }
        }
    }

    if (should_update_pwm) {
        float i_bus = 0.0f;

        PhaseControlLaw<3>::Result control_law_result = ERROR_CONTROLLER_FAILED;
        if (control_law_) {
            control_law_result = control_law_->get_output(last_update_timestamp_ + 2 * TIM_1_8_PERIOD_CLOCKS * (TIM_1_8_RCR + 1));
        }

        // Apply control law to calculate PWM duty cycles
        if (is_armed_ && (control_law_result.index() == 0)) {
            std::array<float, 3> pwm_timings = std::get<0>(control_law_result);

            // Calculate DC power consumption
            // Note that a pwm_timing of 1 corresponds to DC- and 0 corresponds to DC+
            i_bus = (0.5f - pwm_timings[0]) * current_meas_.phA + (0.5f - pwm_timings[1]) * current_meas_.phB + (0.5f - pwm_timings[2]) * current_meas_.phC;

            uint16_t next_timings[] = {
                (uint16_t)(pwm_timings[0] * (float)TIM_1_8_PERIOD_CLOCKS),
                (uint16_t)(pwm_timings[1] * (float)TIM_1_8_PERIOD_CLOCKS),
                (uint16_t)(pwm_timings[2] * (float)TIM_1_8_PERIOD_CLOCKS)
            };

            apply_pwm_timings(next_timings, false);
        } else if (is_armed_) {
            if (!(timer_->Instance->BDTR & TIM_BDTR_MOE) && (control_law_result == PhaseControlLaw<3>::Result{ERROR_CONTROLLER_INITIALIZING})) {
                // If the PWM output is armed in software but not yet in
                // hardware we tolerate the "initializing" error.
            } else {
                disarm_with_error((control_law_result.index() == 1) ? std::get<1>(control_law_result) : ERROR_CONTROLLER_FAILED);
            }
        }

        // If something above failed, reset I_bus to 0A.
        if (!is_armed_) {
            i_bus = 0.0f;
        }

        I_bus_ = i_bus;

        if (i_bus < config_.I_bus_hard_min || i_bus > config_.I_bus_hard_max) {
            disarm_with_error(ERROR_I_BUS_OUT_OF_RANGE);
        }

        update_brake_current();
        log_timing(TIMING_LOG_CTRL_DONE);
    }
}
