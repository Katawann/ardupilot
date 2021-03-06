#include "Copter.h"

#if MODE_FOLLOW_ENABLED == ENABLED

/*
 * mode_follow.cpp - follow another mavlink-enabled vehicle by system id
 *
 * TODO: stick control to move around on sphere
 * TODO: stick control to change sphere diameter
 * TODO: "channel 7 option" to lock onto "pointed at" target
 * TODO: do better in terms of loitering around the moving point; may need a PID?  Maybe use loiter controller somehow?
 * TODO: extrapolate target vehicle position using its velocity and acceleration
 * TODO: ensure AC_AVOID_ENABLED is true because we rely on it velocity limiting functions
 */

#if 1
#define Debug(fmt, args ...)  do {                                      \
        gcs().send_text(MAV_SEVERITY_WARNING, fmt, ## args);            \
    } while(0)
#elif 0
#define Debug(fmt, args ...)  do {                                      \
    ::fprintf(stderr, "%s:%d: " fmt "\n", __FUNCTION__, __LINE__, ## args);
#else
#define Debug(fmt, args ...)
#endif

// initialise follow mode
bool Copter::ModeFollow::init(const bool ignore_checks)
{
    // re-use guided mode
    gcs().send_text(MAV_SEVERITY_WARNING, "Follow-mode init");
    if (!g2.follow.enabled()) {
        return false;
    }
    return Copter::ModeGuided::init(ignore_checks);
}

void Copter::ModeFollow::run()
{
    // if not auto armed or motor interlock not enabled set throttle to zero and exit immediately
    if (!motors->armed() || !ap.auto_armed || !motors->get_interlock()) {
        zero_throttle_and_relax_ac();
        return;
    }

    // re-use guided mode's velocity controller
    // Note: this is safe from interference from GCSs and companion computer's whose guided mode
    //       position and velocity requests will be ignored while the vehicle is not in guided mode

    // variables to be sent to velocity controller
    Vector3f desired_velocity;
    bool use_yaw = false;
    float yaw_cd = 0.0f;

    Vector3f dist_vec;  // vector to lead vehicle
    Vector3f dist_vec_offs;  // vector to lead vehicle + offset
    Vector3f vel_of_target;  // velocity of lead vehicle
    if (g2.follow.get_target_dist_and_vel_ned(dist_vec, dist_vec_offs, vel_of_target)) {
        // debug
        static uint16_t counter = 0;
        counter++;
        if (counter >= 400) {
            counter = 0;
            Debug("dist to veh: %f %f %f", (double)dist_vec.x, (double)dist_vec.y, (double)dist_vec.z);
        }

        // convert dist_vec_offs to cm in NEU
        const Vector3f dist_vec_offs_neu(dist_vec_offs.x * 100.0f, dist_vec_offs.y * 100.0f, -dist_vec_offs.z * 100.0f);

        // calculate desired velocity vector in cm/s in NEU
        const float kp = g2.follow.get_pos_p().kP();
        desired_velocity.x = (vel_of_target.x * 100.0f) + (dist_vec_offs_neu.x * kp);
        desired_velocity.y = (vel_of_target.y * 100.0f) + (dist_vec_offs_neu.y * kp);
        desired_velocity.z = (-vel_of_target.z * 100.0f) + (dist_vec_offs_neu.z * kp);

        // scale desired velocity to stay within horizontal speed limit
        float desired_speed_xy = safe_sqrt(sq(desired_velocity.x) + sq(desired_velocity.y));
        if (!is_zero(desired_speed_xy) && (desired_speed_xy > pos_control->get_speed_xy())) {
            const float scalar_xy = pos_control->get_speed_xy() / desired_speed_xy;
            desired_velocity.x *= scalar_xy;
            desired_velocity.y *= scalar_xy;
            desired_speed_xy = pos_control->get_speed_xy();
        }

        // limit desired velocity to be between maximum climb and descent rates
        desired_velocity.z = constrain_float(desired_velocity.z, -fabsf(pos_control->get_speed_down()), pos_control->get_speed_up());

        // unit vector towards target position (i.e. vector to lead vehicle + offset)
        Vector3f dir_to_target_neu = dist_vec_offs_neu;
        const float dir_to_target_neu_len = dir_to_target_neu.length();
        if (!is_zero(dir_to_target_neu_len)) {
            dir_to_target_neu /= dir_to_target_neu_len;
        }

        // create horizontal desired velocity vector (required for slow down calculations)
        Vector2f desired_velocity_xy(desired_velocity.x, desired_velocity.y);

        // create horizontal unit vector towards target (required for slow down calculations)
        Vector2f dir_to_target_xy(desired_velocity_xy.x, desired_velocity_xy.y);
        if (!dir_to_target_xy.is_zero()) {
            dir_to_target_xy.normalize();
        }

        // slow down horizontally as we approach target (use 1/2 of maximum deceleration for gentle slow down)
        const float dist_to_target_xy = Vector2f(dist_vec_offs_neu.x, dist_vec_offs_neu.y).length();
        copter.avoid.limit_velocity(pos_control->get_pos_xy_p().kP().get(), pos_control->get_accel_xy() * 0.5f, desired_velocity_xy, dir_to_target_xy, dist_to_target_xy, copter.G_Dt);

        // limit the horizontal velocity to prevent fence violations
        copter.avoid.adjust_velocity(pos_control->get_pos_xy_p().kP().get(), pos_control->get_accel_xy(), desired_velocity_xy, G_Dt);

        // copy horizontal velocity limits back to 3d vector
        desired_velocity.x = desired_velocity_xy.x;
        desired_velocity.y = desired_velocity_xy.y;

        // limit vertical desired_velocity to slow as we approach target (we use 1/2 of maximum deceleration for gentle slow down)
        const float des_vel_z_max = copter.avoid.get_max_speed(pos_control->get_pos_z_p().kP().get(), pos_control->get_accel_z() * 0.5f, fabsf(dist_vec_offs_neu.z), copter.G_Dt);
        desired_velocity.z = constrain_float(desired_velocity.z, -des_vel_z_max, des_vel_z_max);

        // get avoidance adjusted climb rate
        desired_velocity.z = get_avoidance_adjusted_climbrate(desired_velocity.z);

        // calculate vehicle heading
        switch (g2.follow.get_yaw_behave()) {
            case AP_Follow::YAW_BEHAVE_FACE_LEAD_VEHICLE: {
                const Vector3f dist_vec_xy(dist_vec.x, dist_vec.y, 0.0f);
                if (dist_vec_xy.length() > 1.0f) {
                    yaw_cd = get_bearing_cd(Vector3f(), dist_vec_xy);
                    use_yaw = true;
                }
                break;
            }

            case AP_Follow::YAW_BEHAVE_SAME_AS_LEAD_VEHICLE: {
                float target_hdg = 0.0f;
                if (g2.follow.get_target_heading(target_hdg)) {
                    yaw_cd = target_hdg * 100.0f;
                    use_yaw = true;
                }
                break;
            }

            case AP_Follow::YAW_BEHAVE_DIR_OF_FLIGHT: {
                const Vector3f vel_vec(desired_velocity.x, desired_velocity.y, 0.0f);
                if (vel_vec.length() > 100.0f) {
                    yaw_cd = get_bearing_cd(Vector3f(), vel_vec);
                    use_yaw = true;
                }
                break;
            }

            case AP_Follow::YAW_BEHAVE_NONE:
            default:
                // do nothing
               break;

        }
    }

    // log output at 10hz
    uint32_t now = AP_HAL::millis();
    bool log_request = false;
    if ((now - last_log_ms >= 100) || (last_log_ms == 0)) {
        log_request = true;
        last_log_ms = now;
    }
    // re-use guided mode's velocity controller (takes NEU)
    Copter::ModeGuided::set_velocity(desired_velocity, use_yaw, yaw_cd, false, 0.0f, false, log_request);

    Copter::ModeGuided::run();
}

#endif // MODE_FOLLOW_ENABLED == ENABLED
