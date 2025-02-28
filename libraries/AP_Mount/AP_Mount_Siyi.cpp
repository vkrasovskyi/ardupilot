#include "AP_Mount_Siyi.h"

#if HAL_MOUNT_SIYI_ENABLED
#include <AP_HAL/AP_HAL.h>
#include <AP_AHRS/AP_AHRS.h>
#include <GCS_MAVLink/GCS.h>
#include <GCS_MAVLink/include/mavlink/v2.0/checksum.h>
#include <AP_SerialManager/AP_SerialManager.h>

extern const AP_HAL::HAL& hal;

#define AP_MOUNT_SIYI_HEADER1       0x55    // first header byte
#define AP_MOUNT_SIYI_HEADER2       0x66    // second header byte
#define AP_MOUNT_SIYI_PACKETLEN_MIN 10      // minimum number of bytes in a packet.  this is a packet with no data bytes
#define AP_MOUNT_SIYI_DATALEN_MAX   (AP_MOUNT_SIYI_PACKETLEN_MAX-AP_MOUNT_SIYI_PACKETLEN_MIN) // max bytes for data portion of packet
#define AP_MOUNT_SIYI_SERIAL_RESEND_MS   1000    // resend angle targets to gimbal once per second
#define AP_MOUNT_SIYI_MSG_BUF_DATA_START 8  // data starts at this byte in _msg_buf
#define AP_MOUNT_SIYI_RATE_MAX_RADS radians(90) // maximum physical rotation rate of gimbal in radans/sec
#define AP_MOUNT_SIYI_PITCH_P       1.50    // pitch controller P gain (converts pitch angle error to target rate)
#define AP_MOUNT_SIYI_YAW_P         1.50    // yaw controller P gain (converts yaw angle error to target rate)
#define AP_MOUNT_SIYI_LOCK_RESEND_COUNT 5   // lock value is resent to gimbal every 5 iterations

#define AP_MOUNT_SIYI_DEBUG 0
#define debug(fmt, args ...) do { if (AP_MOUNT_SIYI_DEBUG) { GCS_SEND_TEXT(MAV_SEVERITY_INFO, "Siyi: " fmt, ## args); } } while (0)

// init - performs any required initialisation for this instance
void AP_Mount_Siyi::init()
{
    const AP_SerialManager& serial_manager = AP::serialmanager();

    _uart = serial_manager.find_serial(AP_SerialManager::SerialProtocol_Gimbal, 0);
    if (_uart != nullptr) {
        _initialised = true;
    }
    AP_Mount_Backend::init();
}

// update mount position - should be called periodically
void AP_Mount_Siyi::update()
{
    // exit immediately if not initialised
    if (!_initialised) {
        return;
    }

    // reading incoming packets from gimbal
    read_incoming_packets();

    // request firmware version during startup at 1hz
    // during regular operation request configuration at 1hz
    uint32_t now_ms = AP_HAL::millis();
    if ((now_ms - _last_send_ms) >= 1000) {
        _last_send_ms = now_ms;
        if (!_got_firmware_version) {
            request_firmware_version();
            return;
        } else {
            request_configuration();
        }
    }

    // request attitude at regular intervals
    if ((now_ms - _last_req_current_angle_rad_ms) >= 50) {
        request_gimbal_attitude();
        _last_req_current_angle_rad_ms = now_ms;
    }

    // run zoom control
    update_zoom_control();

    // update based on mount mode
    switch (get_mode()) {
        // move mount to a "retracted" position.  To-Do: remove support and replace with a relaxed mode?
        case MAV_MOUNT_MODE_RETRACT: {
            const Vector3f &angle_bf_target = _params.retract_angles.get();
            send_target_angles(ToRad(angle_bf_target.y), ToRad(angle_bf_target.z), false);
            break;
        }

        // move mount to a neutral position, typically pointing forward
        case MAV_MOUNT_MODE_NEUTRAL: {
            const Vector3f &angle_bf_target = _params.neutral_angles.get();
            send_target_angles(ToRad(angle_bf_target.y), ToRad(angle_bf_target.z), false);
            break;
        }

        // point to the angles given by a mavlink message
        case MAV_MOUNT_MODE_MAVLINK_TARGETING:
            switch (mavt_target.target_type) {
            case MountTargetType::ANGLE:
                send_target_angles(mavt_target.angle_rad.pitch, mavt_target.angle_rad.yaw, mavt_target.angle_rad.yaw_is_ef);
                break;
            case MountTargetType::RATE:
                send_target_rates(mavt_target.rate_rads.pitch, mavt_target.rate_rads.yaw, mavt_target.rate_rads.yaw_is_ef);
                break;
            }
            break;

        // RC radio manual angle control, but with stabilization from the AHRS
        case MAV_MOUNT_MODE_RC_TARGETING: {
            // update targets using pilot's rc inputs
            MountTarget rc_target {};
            if (get_rc_rate_target(rc_target)) {
                send_target_rates(rc_target.pitch, rc_target.yaw, rc_target.yaw_is_ef);
            } else if (get_rc_angle_target(rc_target)) {
                send_target_angles(rc_target.pitch, rc_target.yaw, rc_target.yaw_is_ef);
            }
            break;
        }

        // point mount to a GPS point given by the mission planner
        case MAV_MOUNT_MODE_GPS_POINT: {
            MountTarget angle_target_rad {};
            if (get_angle_target_to_roi(angle_target_rad)) {
                send_target_angles(angle_target_rad.pitch, angle_target_rad.yaw, angle_target_rad.yaw_is_ef);
            }
            break;
        }

        case MAV_MOUNT_MODE_HOME_LOCATION: {
            MountTarget angle_target_rad {};
            if (get_angle_target_to_home(angle_target_rad)) {
                send_target_angles(angle_target_rad.pitch, angle_target_rad.yaw, angle_target_rad.yaw_is_ef);
            }
            break;
        }

        case MAV_MOUNT_MODE_SYSID_TARGET:{
            MountTarget angle_target_rad {};
            if (get_angle_target_to_sysid(angle_target_rad)) {
                send_target_angles(angle_target_rad.pitch, angle_target_rad.yaw, angle_target_rad.yaw_is_ef);
            }
            break;
        }

        default:
            // we do not know this mode so raise internal error
            INTERNAL_ERROR(AP_InternalError::error_t::flow_of_control);
            break;
    }
}

// return true if healthy
bool AP_Mount_Siyi::healthy() const
{
    // unhealthy until gimbal has been found and replied with firmware version info
    if (!_initialised || !_got_firmware_version) {
        return false;
    }

    // unhealthy if attitude information NOT received recently
    const uint32_t now_ms = AP_HAL::millis();
    if (now_ms - _last_current_angle_rad_ms > 1000) {
        return false;
    }

    // if we get this far return healthy
    return true;
}

// get attitude as a quaternion.  returns true on success
bool AP_Mount_Siyi::get_attitude_quaternion(Quaternion& att_quat)
{
    att_quat.from_euler(_current_angle_rad.x, _current_angle_rad.y, _current_angle_rad.z);
    return true;
}

// reading incoming packets from gimbal and confirm they are of the correct format
// results are held in the _parsed_msg structure
void AP_Mount_Siyi::read_incoming_packets()
{
    // check for bytes on the serial port
    int16_t nbytes = MIN(_uart->available(), 1024U);
    if (nbytes <= 0 ) {
        return;
    }

    // flag to allow cases below to reset parser state
    bool reset_parser = false;

    // process bytes received
    for (int16_t i = 0; i < nbytes; i++) {
        uint8_t b;
        if (!_uart->read(b)) {
            continue;
        }

        _msg_buff[_msg_buff_len++] = b;

        // protect against overly long messages
        if (_msg_buff_len >= AP_MOUNT_SIYI_PACKETLEN_MAX) {
            reset_parser = true;
        }

        // process byte depending upon current state
        switch (_parsed_msg.state) {

        case ParseState::WAITING_FOR_HEADER_LOW:
            if (b == AP_MOUNT_SIYI_HEADER1) {
                _parsed_msg.state = ParseState::WAITING_FOR_HEADER_HIGH;
            } else {
                reset_parser = true;
            }
            break;

        case ParseState::WAITING_FOR_HEADER_HIGH:
            if (b == AP_MOUNT_SIYI_HEADER2) {
                _parsed_msg.state = ParseState::WAITING_FOR_CTRL;
            } else {
                reset_parser = true;
            }
            break;

        case ParseState::WAITING_FOR_CTRL:
            _parsed_msg.state = ParseState::WAITING_FOR_DATALEN_LOW;
            break;

        case ParseState::WAITING_FOR_DATALEN_LOW:
            _parsed_msg.data_len = b;
            _parsed_msg.state = ParseState::WAITING_FOR_DATALEN_HIGH;
            break;

        case ParseState::WAITING_FOR_DATALEN_HIGH:
            _parsed_msg.data_len |= ((uint16_t)b << 8);
            // sanity check data length
            if (_parsed_msg.data_len <= AP_MOUNT_SIYI_DATALEN_MAX) {
                _parsed_msg.state = ParseState::WAITING_FOR_SEQ_LOW;
            } else {
                reset_parser = true;
                debug("data len too large:%u (>%u)", (unsigned)_parsed_msg.data_len, (unsigned)AP_MOUNT_SIYI_DATALEN_MAX);
            }
            break;

        case ParseState::WAITING_FOR_SEQ_LOW:
            _parsed_msg.state = ParseState::WAITING_FOR_SEQ_HIGH;
            break;

        case ParseState::WAITING_FOR_SEQ_HIGH:
            _parsed_msg.state = ParseState::WAITING_FOR_CMDID;
            break;

        case ParseState::WAITING_FOR_CMDID:
            _parsed_msg.command_id = b;
            _parsed_msg.data_bytes_received = 0;
            if (_parsed_msg.data_len > 0) {
                _parsed_msg.state = ParseState::WAITING_FOR_DATA;
            } else {
                _parsed_msg.state = ParseState::WAITING_FOR_CRC_LOW;
            }
            break;

        case ParseState::WAITING_FOR_DATA:
            _parsed_msg.data_bytes_received++;
            if (_parsed_msg.data_bytes_received >= _parsed_msg.data_len) {
                _parsed_msg.state = ParseState::WAITING_FOR_CRC_LOW;
            }
            break;

        case ParseState::WAITING_FOR_CRC_LOW:
            _parsed_msg.crc16 = b;
            _parsed_msg.state = ParseState::WAITING_FOR_CRC_HIGH;
            break;

        case ParseState::WAITING_FOR_CRC_HIGH:
            _parsed_msg.crc16 |= ((uint16_t)b << 8);

            // check crc
            const uint16_t expected_crc = crc16_ccitt(_msg_buff, _msg_buff_len-2, 0);
            if (expected_crc == _parsed_msg.crc16) {
                // successfully received a message, do something with it
                process_packet();
#if AP_MOUNT_SIYI_DEBUG
            } else {
                debug("crc expected:%x got:%x", (unsigned)expected_crc, (unsigned)_parsed_msg.crc16);
#endif
            }
            reset_parser = true;
            break;
        }

        // handle reset of parser
        if (reset_parser) {
            _parsed_msg.state = ParseState::WAITING_FOR_HEADER_LOW;
            _msg_buff_len = 0;
        }
    }
}

// process successfully decoded packets held in the _parsed_msg structure
void AP_Mount_Siyi::process_packet()
{
#if AP_MOUNT_SIYI_DEBUG
    // flag to warn of unexpected data buffer length
    bool unexpected_len = false;
#endif

    // process packet depending upon command id
    switch ((SiyiCommandId)_parsed_msg.command_id) {

    case SiyiCommandId::ACQUIRE_FIRMWARE_VERSION: {
        if (_parsed_msg.data_bytes_received != 12 &&    // ZR10 firmware version reply is 12bytes
            _parsed_msg.data_bytes_received != 8) {     // A8 firmware version reply is 8 bytes
#if AP_MOUNT_SIYI_DEBUG
            unexpected_len = true;
#endif
            break;
        }
        _got_firmware_version = true;

        // set hardware version based on message length
        _hardware_model = (_parsed_msg.data_bytes_received <= 8) ? HardwareModel::A8 : HardwareModel::ZR10;

        // consume and display camera firmware version
        _cam_firmware_version = {
            _msg_buff[_msg_buff_data_start+2],      // firmware major version
            _msg_buff[_msg_buff_data_start+1],      // firmware minor version
            _msg_buff[_msg_buff_data_start+0]       // firmware revision (aka patch)
        };

        gcs().send_text(MAV_SEVERITY_INFO, "Mount: SiyiCam fw:%u.%u.%u",
                (unsigned)_cam_firmware_version.major,          // firmware major version
                (unsigned)_cam_firmware_version.minor,          // firmware minor version
                (unsigned)_cam_firmware_version.patch);         // firmware revision

        // display gimbal info to user
        gcs().send_text(MAV_SEVERITY_INFO, "Mount: Siyi fw:%u.%u.%u",
                (unsigned)_msg_buff[_msg_buff_data_start+6],    // firmware major version
                (unsigned)_msg_buff[_msg_buff_data_start+5],    // firmware minor version
                (unsigned)_msg_buff[_msg_buff_data_start+4]);   // firmware revision

        // display zoom firmware version
#if AP_MOUNT_SIYI_DEBUG
        if (_parsed_msg.data_bytes_received >= 12) {
            debug("Mount: SiyiZoom fw:%u.%u.%u",
                (unsigned)_msg_buff[_msg_buff_data_start+10],    // firmware major version
                (unsigned)_msg_buff[_msg_buff_data_start+9],     // firmware minor version
                (unsigned)_msg_buff[_msg_buff_data_start+8]);    // firmware revision
        }
#endif
        break;
    }

    case SiyiCommandId::HARDWARE_ID:
        // unsupported
        break;

    case SiyiCommandId::AUTO_FOCUS:
#if AP_MOUNT_SIYI_DEBUG
        if (_parsed_msg.data_bytes_received != 1) {
            unexpected_len = true;
            break;
        }
        debug("AutoFocus:%u", (unsigned)_msg_buff[_msg_buff_data_start]);
#endif
        break;

    case SiyiCommandId::MANUAL_ZOOM_AND_AUTO_FOCUS: {
        if (_parsed_msg.data_bytes_received != 2) {
#if AP_MOUNT_SIYI_DEBUG
            unexpected_len = true;
#endif
            break;
        }
        _zoom_mult = UINT16_VALUE(_msg_buff[_msg_buff_data_start+1], _msg_buff[_msg_buff_data_start]) * 0.1;
        debug("ZoomMult:%4.1f", (double)_zoom_mult);
        break;
    }

    case SiyiCommandId::MANUAL_FOCUS:
#if AP_MOUNT_SIYI_DEBUG
        if (_parsed_msg.data_bytes_received != 1) {
            unexpected_len = true;
            break;
        }
        debug("ManualFocus:%u", (unsigned)_msg_buff[_msg_buff_data_start]);
#endif
        break;

    case SiyiCommandId::GIMBAL_ROTATION:
#if AP_MOUNT_SIYI_DEBUG
        if (_parsed_msg.data_bytes_received != 1) {
            unexpected_len = true;
            break;
        }
        debug("GimbRot:%u", (unsigned)_msg_buff[_msg_buff_data_start]);
#endif
        break;

    case SiyiCommandId::CENTER:
#if AP_MOUNT_SIYI_DEBUG
        if (_parsed_msg.data_bytes_received != 1) {
            unexpected_len = true;
            break;
        }
        debug("Center:%u", (unsigned)_msg_buff[_msg_buff_data_start]);
#endif
        break;

    case SiyiCommandId::ACQUIRE_GIMBAL_CONFIG_INFO: {
        // update gimbal's mounting direction
        if (_parsed_msg.data_bytes_received > 5) {
            _gimbal_mounting_dir = (_msg_buff[_msg_buff_data_start+5] == 2) ? GimbalMountingDirection::UPSIDE_DOWN : GimbalMountingDirection::NORMAL;
        }

        // update recording state and warn user of mismatch
        const bool recording = _msg_buff[_msg_buff_data_start+3] > 0;
        if (recording != _last_record_video) {
            gcs().send_text(MAV_SEVERITY_INFO, "Siyi: recording %s", recording ? "ON" : "OFF");
        }
        _last_record_video = recording;
        debug("GimConf hdr:%u rec:%u foll:%u mntdir:%u", (unsigned)_msg_buff[_msg_buff_data_start+1],
                                                         (unsigned)_msg_buff[_msg_buff_data_start+3],
                                                         (unsigned)_msg_buff[_msg_buff_data_start+4],
                                                         (unsigned)_msg_buff[_msg_buff_data_start+5]);
        break;
    }

    case SiyiCommandId::FUNCTION_FEEDBACK_INFO: {
        if (_parsed_msg.data_bytes_received != 1) {
#if AP_MOUNT_SIYI_DEBUG
            unexpected_len = true;
#endif
            break;
        }
        const uint8_t func_feedback_info = _msg_buff[_msg_buff_data_start];
        const char* err_prefix = "Mount: Siyi";
        switch ((FunctionFeedbackInfo)func_feedback_info) {
        case FunctionFeedbackInfo::SUCCESS:
            debug("FnFeedB success");
            break;
        case FunctionFeedbackInfo::FAILED_TO_TAKE_PHOTO:
            gcs().send_text(MAV_SEVERITY_ERROR, "%s failed to take picture", err_prefix);
            break;
        case FunctionFeedbackInfo::HDR_ON:
            debug("HDR on");
            break;
        case FunctionFeedbackInfo::HDR_OFF:
            debug("HDR off");
            break;
        case FunctionFeedbackInfo::FAILED_TO_RECORD_VIDEO:
            gcs().send_text(MAV_SEVERITY_ERROR, "%s failed to record video", err_prefix);
            break;
        default:
            debug("FnFeedB unexpected val:%u", (unsigned)func_feedback_info);
        }
        break;
    }

    case SiyiCommandId::PHOTO:
        // no ack should ever be sent by the gimbal
        break;

    case SiyiCommandId::ACQUIRE_GIMBAL_ATTITUDE: {
        if (_parsed_msg.data_bytes_received != 12) {
#if AP_MOUNT_SIYI_DEBUG
            unexpected_len = true;
#endif
            break;
        }
        _last_current_angle_rad_ms = AP_HAL::millis();
        _current_angle_rad.z = -radians((int16_t)UINT16_VALUE(_msg_buff[_msg_buff_data_start+1], _msg_buff[_msg_buff_data_start]) * 0.1);   // yaw angle
        _current_angle_rad.y = radians((int16_t)UINT16_VALUE(_msg_buff[_msg_buff_data_start+3], _msg_buff[_msg_buff_data_start+2]) * 0.1);  // pitch angle
        _current_angle_rad.x = radians((int16_t)UINT16_VALUE(_msg_buff[_msg_buff_data_start+5], _msg_buff[_msg_buff_data_start+4]) * 0.1);  // roll angle
        //const float yaw_rate_degs = -(int16_t)UINT16_VALUE(_msg_buff[_msg_buff_data_start+7], _msg_buff[_msg_buff_data_start+6]) * 0.1;   // yaw rate
        //const float pitch_rate_deg = (int16_t)UINT16_VALUE(_msg_buff[_msg_buff_data_start+9], _msg_buff[_msg_buff_data_start+8]) * 0.1;   // pitch rate
        //const float roll_rate_deg = (int16_t)UINT16_VALUE(_msg_buff[_msg_buff_data_start+11], _msg_buff[_msg_buff_data_start+10]) * 0.1;  // roll rate
        break;
    }

    default:
        debug("Unhandled CmdId:%u", (unsigned)_parsed_msg.command_id);
        break;
    }

#if AP_MOUNT_SIYI_DEBUG
    // handle unexpected data buffer length
    if (unexpected_len) {
        debug("CmdId:%u unexpected len:%u", (unsigned)_parsed_msg.command_id, (unsigned)_parsed_msg.data_bytes_received);
    }
#endif
}

// methods to send commands to gimbal
// returns true on success, false if outgoing serial buffer is full
bool AP_Mount_Siyi::send_packet(SiyiCommandId cmd_id, const uint8_t* databuff, uint8_t databuff_len)
{
    if (!_initialised) {
        return false;
    }
    // calculate and sanity check packet size
    const uint16_t packet_size = AP_MOUNT_SIYI_PACKETLEN_MIN + databuff_len;
    if (packet_size > AP_MOUNT_SIYI_PACKETLEN_MAX) {
        debug("send_packet data buff too large");
        return false;
    }

    // check for sufficient space in outgoing buffer
    if (_uart->txspace() < packet_size) {
        return false;
    }

    // buffer for holding outgoing packet
    uint8_t send_buff[packet_size];
    uint8_t send_buff_ofs = 0;

    // packet header
    send_buff[send_buff_ofs++] = AP_MOUNT_SIYI_HEADER1;
    send_buff[send_buff_ofs++] = AP_MOUNT_SIYI_HEADER2;

    // CTRL.  Always request ACK
    send_buff[send_buff_ofs++] = 1;

    // Data_len.  protocol supports uint16_t but messages are never longer than 22 bytes
    send_buff[send_buff_ofs++] = databuff_len;
    send_buff[send_buff_ofs++] = 0;

    // SEQ (sequence)
    send_buff[send_buff_ofs++] = LOWBYTE(_last_seq);
    send_buff[send_buff_ofs++] = HIGHBYTE(_last_seq++);

    // CMD_ID
    send_buff[send_buff_ofs++] = (uint8_t)cmd_id;

    // DATA
    if (databuff_len != 0) {
        memcpy(&send_buff[send_buff_ofs], databuff, databuff_len);
        send_buff_ofs += databuff_len;
    }

    // CRC16
    const uint16_t crc = crc16_ccitt(send_buff, send_buff_ofs, 0);
    send_buff[send_buff_ofs++] = LOWBYTE(crc);
    send_buff[send_buff_ofs++] = HIGHBYTE(crc);

    // send packet
    _uart->write(send_buff, send_buff_ofs);

    return true;
}

// send a packet with a single data byte to gimbal
// returns true on success, false if outgoing serial buffer is full
bool AP_Mount_Siyi::send_1byte_packet(SiyiCommandId cmd_id, uint8_t data_byte)
{
    return send_packet(cmd_id, &data_byte, 1);
}

// rotate gimbal.  pitch_rate and yaw_rate are scalars in the range -100 ~ +100
// yaw_is_ef should be true if gimbal should maintain an earth-frame target (aka lock)
void AP_Mount_Siyi::rotate_gimbal(int8_t pitch_scalar, int8_t yaw_scalar, bool yaw_is_ef)
{
    // send lock/follow value if it has changed
    if ((yaw_is_ef != _last_lock) || (_lock_send_counter >= AP_MOUNT_SIYI_LOCK_RESEND_COUNT)) {
        set_lock(yaw_is_ef);
        _lock_send_counter = 0;
        _last_lock = yaw_is_ef;
    } else {
        _lock_send_counter++;
    }

    const uint8_t yaw_and_pitch_rates[] {(uint8_t)yaw_scalar, (uint8_t)pitch_scalar};
    send_packet(SiyiCommandId::GIMBAL_ROTATION, yaw_and_pitch_rates, ARRAY_SIZE(yaw_and_pitch_rates));
}

// set gimbal's lock vs follow mode
// lock should be true if gimbal should maintain an earth-frame target
// lock is false to follow / maintain a body-frame target
void AP_Mount_Siyi::set_lock(bool lock)
{
    send_1byte_packet(SiyiCommandId::PHOTO, lock ? (uint8_t)PhotoFunction::LOCK_MODE : (uint8_t)PhotoFunction::FOLLOW_MODE);
}

// send target pitch and yaw rates to gimbal
// yaw_is_ef should be true if yaw_rads target is an earth frame rate, false if body_frame
void AP_Mount_Siyi::send_target_rates(float pitch_rads, float yaw_rads, bool yaw_is_ef)
{
    const float pitch_rate_scalar = constrain_float(100.0 * pitch_rads / AP_MOUNT_SIYI_RATE_MAX_RADS, -100, 100);
    const float yaw_rate_scalar = constrain_float(100.0 * yaw_rads / AP_MOUNT_SIYI_RATE_MAX_RADS, -100, 100);
    rotate_gimbal(pitch_rate_scalar, yaw_rate_scalar, yaw_is_ef);
}

// send target pitch and yaw angles to gimbal
// yaw_is_ef should be true if yaw_rad target is an earth frame angle, false if body_frame
void AP_Mount_Siyi::send_target_angles(float pitch_rad, float yaw_rad, bool yaw_is_ef)
{
    // stop gimbal if no recent actual angles
    uint32_t now_ms = AP_HAL::millis();
    if (now_ms - _last_current_angle_rad_ms >= 200) {
        rotate_gimbal(0, 0, false);
        return;
    }

    // if gimbal mounting direction is 2 i.e. upside down, then transform the angles
    Vector3f current_angle_transformed = _current_angle_rad;
    if (_gimbal_mounting_dir == GimbalMountingDirection::UPSIDE_DOWN) {
        current_angle_transformed.y = -wrap_PI(_current_angle_rad.y + M_PI);
        current_angle_transformed.z = -_current_angle_rad.z;
    }

    // use simple P controller to convert pitch angle error (in radians) to a target rate scalar (-100 to +100)
    const float pitch_err_rad = (pitch_rad - current_angle_transformed.y);
    const float pitch_rate_scalar = constrain_float(100.0 * pitch_err_rad * AP_MOUNT_SIYI_PITCH_P / AP_MOUNT_SIYI_RATE_MAX_RADS, -100, 100);

    // convert yaw angle to body-frame
    float yaw_bf_rad = yaw_is_ef ? wrap_PI(yaw_rad - AP::ahrs().yaw) : yaw_rad;

    // enforce body-frame yaw angle limits.  If beyond limits always use body-frame control
    const float yaw_bf_min = radians(_params.yaw_angle_min);
    const float yaw_bf_max = radians(_params.yaw_angle_max);
    if (yaw_bf_rad < yaw_bf_min || yaw_bf_rad > yaw_bf_max) {
        yaw_bf_rad = constrain_float(yaw_bf_rad, yaw_bf_min, yaw_bf_max);
        yaw_is_ef = false;
    }

    // use simple P controller to convert yaw angle error to a target rate scalar (-100 to +100)
    const float yaw_err_rad = (yaw_bf_rad - current_angle_transformed.z);
    const float yaw_rate_scalar = constrain_float(100.0 * yaw_err_rad * AP_MOUNT_SIYI_YAW_P / AP_MOUNT_SIYI_RATE_MAX_RADS, -100, 100);

    // rotate gimbal.  pitch_rate and yaw_rate are scalars in the range -100 ~ +100
    rotate_gimbal(pitch_rate_scalar, yaw_rate_scalar, yaw_is_ef);
}

// take a picture.  returns true on success
bool AP_Mount_Siyi::take_picture()
{
    return send_1byte_packet(SiyiCommandId::PHOTO, (uint8_t)PhotoFunction::TAKE_PICTURE);
}

// start or stop video recording.  returns true on success
// set start_recording = true to start record, false to stop recording
bool AP_Mount_Siyi::record_video(bool start_recording)
{
    // exit immediately if not initialised to reduce mismatch
    // between internal and actual state of recording
    if (!_initialised) {
        return false;
    }

    // check desired recording state has changed
    bool ret = true;
    if (_last_record_video != start_recording) {
        // request recording start or stop (sadly the same message is used)
        const uint8_t func_type = (uint8_t)PhotoFunction::RECORD_VIDEO_TOGGLE;
        ret = send_packet(SiyiCommandId::PHOTO, &func_type, 1);
    }

    // request recording state update from gimbal
    request_configuration();

    return ret;
}

// send zoom rate command to camera. zoom out = -1, hold = 0, zoom in = 1
bool AP_Mount_Siyi::send_zoom_rate(float zoom_value)
{
    uint8_t zoom_step = 0;
    if (zoom_value > 0) {
        // zoom in
        zoom_step = 1;
    }
    if (zoom_value < 0) {
        // zoom out. Siyi API specifies -1 should be sent as 255
        zoom_step = UINT8_MAX;
    }
    return send_1byte_packet(SiyiCommandId::MANUAL_ZOOM_AND_AUTO_FOCUS, zoom_step);
}

// send zoom multiple command to camera. e.g. 1x, 10x, 30x
// only works on ZR10 and ZR30
bool AP_Mount_Siyi::send_zoom_mult(float zoom_mult)
{
    // separate zoom_mult into integral and fractional parts
    float intpart;
    uint8_t fracpart = (uint8_t)constrain_int16(modf(zoom_mult, &intpart) * 10, 0, UINT8_MAX);

    // create and send 2 byte array
    const uint8_t zoom_mult_data[] {(uint8_t)(intpart), fracpart};
    return send_packet(SiyiCommandId::ABSOLUTE_ZOOM, zoom_mult_data, ARRAY_SIZE(zoom_mult_data));
}

// get zoom multiple max
float AP_Mount_Siyi::get_zoom_mult_max() const
{
    switch (_hardware_model) {
    case HardwareModel::UNKNOWN:
        return 0;
    case HardwareModel::A8:
        // a8 has 6x digital zoom
        return 6;
    case HardwareModel::ZR10:
        // zr10 has 30x hybrid zoom (optical + digital)
        return 30;
    }
    return 0;
}

// set zoom specified as a rate or percentage
bool AP_Mount_Siyi::set_zoom(ZoomType zoom_type, float zoom_value)
{
    if (zoom_type == ZoomType::RATE) {
        // disable absolute zoom target
        _zoom_mult_target = 0;
        return send_zoom_rate(zoom_value);
    }

    // absolute zoom
    if (zoom_type == ZoomType::PCT) {
        float zoom_mult_max = get_zoom_mult_max();
        if (is_positive(zoom_mult_max)) {
            // convert zoom percentage (0~100) to target zoom multiple (e.g. 0~6x or 0~30x)
            const float zoom_mult = linear_interpolate(1, zoom_mult_max, zoom_value, 0, 100);
            switch (_hardware_model) {
            case HardwareModel::UNKNOWN:
                // unknown model
                return false;
            case HardwareModel::A8:
                // set internal zoom control target
                _zoom_mult_target = zoom_mult;
                return true;
            case HardwareModel::ZR10:
                return send_zoom_mult(zoom_mult);
            }
        }
    }

    // unsupported zoom type
    return false;
}

// update absolute zoom controller
// only used for A8 that does not support abs zoom control
void AP_Mount_Siyi::update_zoom_control()
{
    // exit immediately if no target
    if (!is_positive(_zoom_mult_target)) {
        return;
    }

    // limit update rate to 20hz
    const uint32_t now_ms = AP_HAL::millis();
    if ((now_ms - _last_zoom_control_ms) <= 50) {
        return;
    }
    _last_zoom_control_ms = now_ms;

    // zoom towards target zoom multiple
    if (_zoom_mult_target > _zoom_mult + 0.1f) {
        send_zoom_rate(1);
    } else if (_zoom_mult_target < _zoom_mult - 0.1f) {
        send_zoom_rate(-1);
    } else {
        send_zoom_rate(0);
        _zoom_mult_target = 0;
    }

    debug("Siyi zoom targ:%f act:%f", (double)_zoom_mult_target, (double)_zoom_mult);
}

// set focus specified as rate, percentage or auto
// focus in = -1, focus hold = 0, focus out = 1
SetFocusResult AP_Mount_Siyi::set_focus(FocusType focus_type, float focus_value)
{
    switch (focus_type) {
    case FocusType::RATE: {
        uint8_t focus_step = 0;
        if (focus_value > 0) {
            focus_step = 1;
        } else if (focus_value < 0) {
            // Siyi API specifies -1 should be sent as 255
            focus_step = UINT8_MAX;
        }
        if (!send_1byte_packet(SiyiCommandId::MANUAL_FOCUS, (uint8_t)focus_step)) {
            return SetFocusResult::FAILED;
        }
        return SetFocusResult::ACCEPTED;
    }
    case FocusType::PCT:
        // not supported
        return SetFocusResult::INVALID_PARAMETERS;
    case FocusType::AUTO:
        if (!send_1byte_packet(SiyiCommandId::AUTO_FOCUS, 1)) {
            return SetFocusResult::FAILED;
        }
        return SetFocusResult::ACCEPTED;
    }

    // unsupported focus type
    return SetFocusResult::INVALID_PARAMETERS;
}

// send camera information message to GCS
void AP_Mount_Siyi::send_camera_information(mavlink_channel_t chan) const
{
    // exit immediately if not initialised
    if (!_initialised || !_got_firmware_version) {
        return;
    }

    static const uint8_t vendor_name[32] = "Siyi";
    static uint8_t model_name[32] = "Unknown";
    const uint32_t fw_version = _cam_firmware_version.major | (_cam_firmware_version.minor << 8) | (_cam_firmware_version.patch << 16);
    const char cam_definition_uri[140] {};

    // focal length
    float focal_length_mm = 0;
    switch (_hardware_model) {
    case HardwareModel::UNKNOWN:
        break;
    case HardwareModel::A8:
        strncpy((char *)model_name, "A8", sizeof(model_name));
        focal_length_mm = 21;
        break;
    case HardwareModel::ZR10:
        strncpy((char *)model_name, "ZR10", sizeof(model_name));
        // focal length range from 5.15 ~ 47.38
        focal_length_mm = 5.15;
        break;
    }

    // capability flags
    const uint32_t flags = CAMERA_CAP_FLAGS_CAPTURE_VIDEO |
                           CAMERA_CAP_FLAGS_CAPTURE_IMAGE |
                           CAMERA_CAP_FLAGS_HAS_BASIC_ZOOM |
                           CAMERA_CAP_FLAGS_HAS_BASIC_FOCUS;

    // send CAMERA_INFORMATION message
    mavlink_msg_camera_information_send(
        chan,
        AP_HAL::millis(),       // time_boot_ms
        vendor_name,            // vendor_name uint8_t[32]
        model_name,             // model_name uint8_t[32]
        fw_version,             // firmware version uint32_t
        focal_length_mm,        // focal_length float (mm)
        0,                      // sensor_size_h float (mm)
        0,                      // sensor_size_v float (mm)
        0,                      // resolution_h uint16_t (pix)
        0,                      // resolution_v uint16_t (pix)
        0,                      // lens_id uint8_t
        flags,                  // flags uint32_t (CAMERA_CAP_FLAGS)
        0,                      // cam_definition_version uint16_t
        cam_definition_uri);    // cam_definition_uri char[140]
}

// send camera settings message to GCS
void AP_Mount_Siyi::send_camera_settings(mavlink_channel_t chan) const
{
    const float NaN = nanf("0x4152");
    const float zoom_mult_max = get_zoom_mult_max();
    const float zoom_pct = is_positive(zoom_mult_max) ? (_zoom_mult / zoom_mult_max * 100) : 0;

    // send CAMERA_SETTINGS message
    mavlink_msg_camera_settings_send(
        chan,
        AP_HAL::millis(),   // time_boot_ms
        _last_record_video ? CAMERA_MODE_VIDEO : CAMERA_MODE_IMAGE, // camera mode (0:image, 1:video, 2:image survey)
        zoom_pct,           // zoomLevel float, percentage from 0 to 100, NaN if unknown
        NaN);               // focusLevel float, percentage from 0 to 100, NaN if unknown
}

#endif // HAL_MOUNT_SIYI_ENABLED
