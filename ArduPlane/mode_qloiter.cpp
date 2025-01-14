#include "mode.h"
#include "Plane.h"

bool ModeQLoiter::_enter()
{
    // initialise loiter
    loiter_nav->clear_pilot_desired_acceleration();
    loiter_nav->init_target();

    // set vertical speed and acceleration limits
    pos_control->set_max_speed_accel_z(-quadplane.get_pilot_velocity_z_max_dn(), quadplane.pilot_velocity_z_max_up, quadplane.pilot_accel_z);
    pos_control->set_correction_speed_accel_z(-quadplane.get_pilot_velocity_z_max_dn(), quadplane.pilot_velocity_z_max_up, quadplane.pilot_accel_z);

    quadplane.init_throttle_wait();

    // remember initial pitch
    quadplane.loiter_initial_pitch_cd = MAX(plane.ahrs.pitch_sensor, 0);

    // prevent re-init of target position
    quadplane.last_loiter_ms = AP_HAL::millis();
    return true;
}

void ModeQLoiter::update()
{
    plane.mode_qstabilize.update();
}

// run quadplane loiter controller
void ModeQLoiter::run()
{
    if (quadplane.throttle_wait) {
        quadplane.set_desired_spool_state(AP_Motors::DesiredSpoolState::GROUND_IDLE);
        attitude_control->set_throttle_out(0, true, 0);
        quadplane.relax_attitude_control();
        pos_control->relax_z_controller(0);
        loiter_nav->clear_pilot_desired_acceleration();
        loiter_nav->init_target();
        return;
    }
    if (!quadplane.motors->armed()) {
        plane.mode_qloiter._enter();
    }

    quadplane.check_attitude_relax();

    if (quadplane.should_relax()) {
        loiter_nav->soften_for_landing();
    }

    const uint32_t now = AP_HAL::millis();
    if (now - quadplane.last_loiter_ms > 500) {
        loiter_nav->clear_pilot_desired_acceleration();
        loiter_nav->init_target();
    }
    quadplane.last_loiter_ms = now;

    // motors use full range
    quadplane.set_desired_spool_state(AP_Motors::DesiredSpoolState::THROTTLE_UNLIMITED);

    // set vertical speed and acceleration limits
    pos_control->set_max_speed_accel_z(-quadplane.get_pilot_velocity_z_max_dn(), quadplane.pilot_velocity_z_max_up, quadplane.pilot_accel_z);

    // process pilot's roll and pitch input
    float target_roll_cd, target_pitch_cd;
    quadplane.get_pilot_desired_lean_angles(target_roll_cd, target_pitch_cd, loiter_nav->get_angle_max_cd(), attitude_control->get_althold_lean_angle_max());
    loiter_nav->set_pilot_desired_acceleration(target_roll_cd, target_pitch_cd);
    
    // run loiter controller
    if (!pos_control->is_active_xy()) {
        pos_control->init_xy_controller();
    }
    loiter_nav->update();

    // nav roll and pitch are controller by loiter controller
    plane.nav_roll_cd = loiter_nav->get_roll();
    plane.nav_pitch_cd = loiter_nav->get_pitch();

    if (now - quadplane.last_pidz_init_ms < (uint32_t)quadplane.transition_time_ms*2 && !quadplane.tailsitter.enabled()) {
        // we limit pitch during initial transition
        float pitch_limit_cd = linear_interpolate(quadplane.loiter_initial_pitch_cd, quadplane.aparm.angle_max,
                                                  now,
                                                  quadplane.last_pidz_init_ms, quadplane.last_pidz_init_ms+quadplane.transition_time_ms*2);
        if (plane.nav_pitch_cd > pitch_limit_cd) {
            plane.nav_pitch_cd = pitch_limit_cd;
            pos_control->set_externally_limited_xy();
        }
    }
    
    
    // call attitude controller with conservative smoothing gain of 4.0f
    attitude_control->input_euler_angle_roll_pitch_euler_rate_yaw(plane.nav_roll_cd,
                                                                  plane.nav_pitch_cd,
                                                                  quadplane.get_desired_yaw_rate_cds());

    if (plane.control_mode == &plane.mode_qland) {
        if (poscontrol.get_state() < QuadPlane::QPOS_LAND_FINAL && quadplane.check_land_final()) {
            poscontrol.set_state(QuadPlane::QPOS_LAND_FINAL);
            quadplane.setup_target_position();
            // cut IC engine if enabled
            if (quadplane.land_icengine_cut != 0) {
                plane.g2.ice_control.engine_control(0, 0, 0);
            }
        }
        float height_above_ground = plane.relative_ground_altitude(plane.g.rangefinder_landing);
        float descent_rate_cms = quadplane.landing_descent_rate_cms(height_above_ground);

        if (poscontrol.get_state() == QuadPlane::QPOS_LAND_FINAL && (quadplane.options & QuadPlane::OPTION_DISABLE_GROUND_EFFECT_COMP) == 0) {
            quadplane.ahrs.set_touchdown_expected(true);
        }

        quadplane.set_climb_rate_cms(-descent_rate_cms, descent_rate_cms>0);
        quadplane.check_land_complete();
    } else if (plane.control_mode == &plane.mode_guided && quadplane.guided_takeoff) {
        quadplane.set_climb_rate_cms(0, false);
    } else {
        // update altitude target and call position controller
        quadplane.set_climb_rate_cms(quadplane.get_pilot_desired_climb_rate_cms(), false);
    }
    quadplane.run_z_controller();
}

