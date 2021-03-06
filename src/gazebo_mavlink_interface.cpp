/*
 * Copyright 2015 Fadri Furrer, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Michael Burri, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Mina Kamel, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Janosch Nikolic, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Markus Achtelik, ASL, ETH Zurich, Switzerland
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "gazebo_mavlink_interface.h"

#define UDP_PORT 14560

namespace gazebo {

GazeboMavlinkInterface::~GazeboMavlinkInterface() {
  event::Events::DisconnectWorldUpdateBegin(updateConnection_);
}

void GazeboMavlinkInterface::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf) {
  // Store the pointer to the model.
  model_ = _model;

  world_ = model_->GetWorld();

  namespace_.clear();

  if (_sdf->HasElement("robotNamespace"))
    namespace_ = _sdf->GetElement("robotNamespace")->Get<std::string>();
  else
    gzerr << "[gazebo_mavlink_interface] Please specify a robotNamespace.\n";

  node_handle_ = transport::NodePtr(new transport::Node());
  node_handle_->Init(namespace_);

  getSdfParam<std::string>(_sdf, "motorSpeedCommandPubTopic", motor_velocity_reference_pub_topic_,
                           motor_velocity_reference_pub_topic_);

  // Listen to the update event. This event is broadcast every
  // simulation iteration.
  updateConnection_ = event::Events::ConnectWorldUpdateBegin(
      boost::bind(&GazeboMavlinkInterface::OnUpdate, this, _1));

  // Subscriber to IMU sensor_msgs::Imu Message and SITL's HilControl message
  mav_control_sub_ = node_handle_->Subscribe(mavlink_control_sub_topic_, &GazeboMavlinkInterface::HilControlCallback, this);
  imu_sub_ = node_handle_->Subscribe(imu_sub_topic_, &GazeboMavlinkInterface::ImuCallback, this);
  
  // Publish HilSensor Message and gazebo's motor_speed message
  motor_velocity_reference_pub_ = node_handle_->Advertise<mav_msgs::msgs::CommandMotorSpeed>(motor_velocity_reference_pub_topic_, 10);
  hil_sensor_pub_ = node_handle_->Advertise<mavlink::msgs::HilSensor>(hil_sensor_mavlink_pub_topic_, 10);
  hil_gps_pub_ = node_handle_->Advertise<mavlink::msgs::HilGps>(hil_gps_mavlink_pub_topic_);

  _rotor_count = 4;
  last_time_ = world_->GetSimTime();
  last_gps_time_ = world_->GetSimTime();
  double gps_update_interval_ = 200*1000000;  // nanoseconds for 5Hz

  gravity_W_ = world_->GetPhysicsEngine()->GetGravity();

  // Magnetic field data for Zurich from WMM2015 (10^5xnanoTesla (N, E, D))
  mag_W_ = {0.21523, 0.00771, 0.42741};

  //Create socket
  // udp socket data
  const int _port = UDP_PORT;

  // try to setup udp socket for communcation with simulator
  if ((_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    printf("create socket failed\n");
    return;
  }

  _srcaddr.sin_family = AF_INET;
  _srcaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  _srcaddr.sin_port = htons(UDP_PORT);

  _addrlen = sizeof(_srcaddr);

  fds[0].fd = _fd;
  fds[0].events = POLLIN;
}

// This gets called by the world update start event.
void GazeboMavlinkInterface::OnUpdate(const common::UpdateInfo& /*_info*/) {

  pollForMAVLinkMessages();

  if(!received_first_referenc_)
    return;

  common::Time now = world_->GetSimTime();

  mav_msgs::msgs::CommandMotorSpeed turning_velocities_msg;


  for (int i = 0; i < input_reference_.size(); i++){
    turning_velocities_msg.add_motor_speed(input_reference_[i]);
  }
  // TODO Add timestamp and Header
  // turning_velocities_msg->header.stamp.sec = now.sec;
  // turning_velocities_msg->header.stamp.nsec = now.nsec;

  motor_velocity_reference_pub_->Publish(turning_velocities_msg);

  //send gps
  common::Time current_time  = now;
  double dt = (current_time - last_time_).Double();
  last_time_ = current_time;
  double t = current_time.Double();

  math::Pose T_W_I = model_->GetWorldPose(); //TODO(burrimi): Check tf.
  math::Vector3 pos_W_I = T_W_I.pos;  // Use the models' world position for GPS and pressure alt.

  math::Vector3 velocity_current_W = model_->GetWorldLinearVel();  // Use the models' world position for GPS velocity.

  math::Vector3 velocity_current_W_xy = velocity_current_W;
  velocity_current_W_xy.z = 0.0;

  // TODO: Remove GPS message from IMU plugin. Added gazebo GPS plugin. This is temp here.
  float lat_zurich = 47.3667;  // deg
  float long_zurich = 8.5500;  // deg
  float earth_radius = 6353000;  // m
  
  common::Time gps_update(gps_update_interval_);

  if(current_time - last_gps_time_ > gps_update){  // 5Hz

    if(use_mavlink_udp){
      // Raw UDP mavlink
      mavlink_hil_gps_t hil_gps_msg;
      hil_gps_msg.time_usec = current_time.nsec*1000;
      hil_gps_msg.fix_type = 3;
      hil_gps_msg.lat = (lat_zurich + (pos_W_I.x/earth_radius)*180/3.1416) * 10000000;
      hil_gps_msg.lon = (long_zurich + (-pos_W_I.y/earth_radius)*180/3.1416) * 10000000;
      hil_gps_msg.alt = pos_W_I.z * 1000;
      hil_gps_msg.eph = 100;
      hil_gps_msg.epv = 100;
      hil_gps_msg.vel = velocity_current_W_xy.GetLength() * 100;
      hil_gps_msg.vn = velocity_current_W.x * 100;
      hil_gps_msg.ve = -velocity_current_W.y * 100;
      hil_gps_msg.vd = -velocity_current_W.z * 100;
      hil_gps_msg.cog = atan2(hil_gps_msg.ve, hil_gps_msg.vn) * 180.0/3.1416 * 100.0;
      hil_gps_msg.satellites_visible = 10;

      send_mavlink_message(MAVLINK_MSG_ID_HIL_GPS, &hil_gps_msg, 200);
    } else{
      // Send via protobuf
      hil_gps_msg_.set_time_usec(current_time.nsec*1000);
      hil_gps_msg_.set_fix_type(3);
      hil_gps_msg_.set_lat((lat_zurich + (pos_W_I.x/earth_radius)*180/3.1416) * 10000000);
      hil_gps_msg_.set_lon((long_zurich + (-pos_W_I.y/earth_radius)*180/3.1416) * 10000000);
      hil_gps_msg_.set_alt(pos_W_I.z * 1000);
      hil_gps_msg_.set_eph(100);
      hil_gps_msg_.set_epv(100);
      hil_gps_msg_.set_vel(velocity_current_W_xy.GetLength() * 100);
      hil_gps_msg_.set_vn(velocity_current_W.x * 100);
      hil_gps_msg_.set_ve(-velocity_current_W.y * 100);
      hil_gps_msg_.set_vd(-velocity_current_W.z * 100);
      hil_gps_msg_.set_cog(atan2(-velocity_current_W.y * 100, velocity_current_W.x * 100) * 180.0/3.1416 * 100.0);
      hil_gps_msg_.set_satellites_visible(10);
             
      hil_gps_pub_->Publish(hil_gps_msg_);
    }

    last_gps_time_ = current_time;
  }
}

void GazeboMavlinkInterface::HilControlCallback(HilControlPtr &rmsg) {
  if(!use_mavlink_udp){

    inputs.control[0] =(double)rmsg->roll_ailerons();
    inputs.control[1] =(double)rmsg->pitch_elevator();
    inputs.control[2] =(double)rmsg->yaw_rudder();
    inputs.control[3] =(double)rmsg->throttle();
    inputs.control[4] =(double)rmsg->aux1();
    inputs.control[5] =(double)rmsg->aux2();
    inputs.control[6] =(double)rmsg->aux3();
    inputs.control[7] =(double)rmsg->aux4();

    // publish message
    double scaling = 150;
    double offset = 600;

    mav_msgs::msgs::CommandMotorSpeed* turning_velocities_msg = new mav_msgs::msgs::CommandMotorSpeed;

    for (int i = 0; i < _rotor_count; i++) {
      turning_velocities_msg->add_motor_speed(inputs.control[i] * scaling + offset);
    }

    input_reference_.resize(turning_velocities_msg->motor_speed_size());
    for (int i = 0; i < turning_velocities_msg->motor_speed_size(); ++i) {
      input_reference_[i] = turning_velocities_msg->motor_speed(i);
    }
    received_first_referenc_ = true;
  }
}

void GazeboMavlinkInterface::send_mavlink_message(const uint8_t msgid, const void *msg, uint8_t component_ID) {
  component_ID = 0;
  uint8_t payload_len = mavlink_message_lengths[msgid];
  unsigned packet_len = payload_len + MAVLINK_NUM_NON_PAYLOAD_BYTES;

  uint8_t buf[MAVLINK_MAX_PACKET_LEN];

  /* header */
  buf[0] = MAVLINK_STX;
  buf[1] = payload_len;
  /* no idea which numbers should be here*/
  buf[2] = 100;
  buf[3] = 0;
  buf[4] = component_ID;
  buf[5] = msgid;

  /* payload */
  memcpy(&buf[MAVLINK_NUM_HEADER_BYTES],msg, payload_len);

  /* checksum */
  uint16_t checksum;
  crc_init(&checksum);
  crc_accumulate_buffer(&checksum, (const char *) &buf[1], MAVLINK_CORE_HEADER_LEN + payload_len);
  crc_accumulate(mavlink_message_crcs[msgid], &checksum);

  buf[MAVLINK_NUM_HEADER_BYTES + payload_len] = (uint8_t)(checksum & 0xFF);
  buf[MAVLINK_NUM_HEADER_BYTES + payload_len + 1] = (uint8_t)(checksum >> 8);

  ssize_t len = sendto(_fd, buf, packet_len, 0, (struct sockaddr *)&_srcaddr, sizeof(_srcaddr));
  if (len <= 0) {
    printf("Failed sending mavlink message");
  }
}

void GazeboMavlinkInterface::ImuCallback(ImuPtr& imu_message) {

  math::Pose T_W_I = model_->GetWorldPose();
  math::Vector3 pos_W_I = T_W_I.pos;  // Use the models'world position for GPS and pressure alt.

  math::Quaternion C_W_I;
  C_W_I.w = imu_message->orientation().w();
  C_W_I.x = imu_message->orientation().x();
  C_W_I.y = imu_message->orientation().y();
  C_W_I.z = imu_message->orientation().z();

  math::Vector3 mag_I = C_W_I.RotateVectorReverse(mag_W_); // TODO: Add noise based on bais and variance like for imu and gyro
  math::Vector3 body_vel = C_W_I.RotateVectorReverse(model_->GetWorldLinearVel());
  
  if(use_mavlink_udp){
    mavlink_hil_sensor_t sensor_msg;
    sensor_msg.time_usec = world_->GetSimTime().nsec*1000;
    sensor_msg.xacc = imu_message->linear_acceleration().x();
    sensor_msg.yacc = imu_message->linear_acceleration().y();
    sensor_msg.zacc = imu_message->linear_acceleration().z();
    sensor_msg.xgyro = imu_message->angular_velocity().x();
    sensor_msg.ygyro = imu_message->angular_velocity().y();
    sensor_msg.zgyro = imu_message->angular_velocity().z();
    sensor_msg.xmag = mag_I.x;
    sensor_msg.ymag = mag_I.y;
    sensor_msg.zmag = mag_I.z;
    sensor_msg.abs_pressure = 0.0;
    sensor_msg.diff_pressure = 0.5*1.2754*body_vel.x*body_vel.x;
    sensor_msg.pressure_alt = pos_W_I.z;
    sensor_msg.temperature = 0.0;
    sensor_msg.fields_updated = 4095;

    send_mavlink_message(MAVLINK_MSG_ID_HIL_SENSOR, &sensor_msg, 200);    
  } else{
    hil_sensor_msg_.set_time_usec(world_->GetSimTime().nsec*1000);
    hil_sensor_msg_.set_xacc(imu_message->linear_acceleration().x());
    hil_sensor_msg_.set_yacc(imu_message->linear_acceleration().y());
    hil_sensor_msg_.set_zacc(imu_message->linear_acceleration().z());
    hil_sensor_msg_.set_xgyro(imu_message->angular_velocity().x());
    hil_sensor_msg_.set_ygyro(imu_message->angular_velocity().y());
    hil_sensor_msg_.set_zgyro(imu_message->angular_velocity().z());
    hil_sensor_msg_.set_xmag(mag_I.x);
    hil_sensor_msg_.set_ymag(mag_I.y);
    hil_sensor_msg_.set_zmag(mag_I.z);
    hil_sensor_msg_.set_abs_pressure(0.0);
    hil_sensor_msg_.set_diff_pressure(0.5*1.2754*body_vel.x*body_vel.x);
    hil_sensor_msg_.set_pressure_alt(pos_W_I.z);
    hil_sensor_msg_.set_temperature(0.0);
    hil_sensor_msg_.set_fields_updated(4095);  // 0b1111111111111 (All updated since new data with new noise added always)
    
    hil_sensor_pub_->Publish(hil_sensor_msg_);
  }
}

void GazeboMavlinkInterface::pollForMAVLinkMessages()
{
  int len;
  ::poll(&fds[0], (sizeof(fds[0])/sizeof(fds[0])), 0);
  if (fds[0].revents & POLLIN) {
    len = recvfrom(_fd, _buf, sizeof(_buf), 0, (struct sockaddr *)&_srcaddr, &_addrlen);
    if (len > 0) {
      mavlink_message_t msg;
      mavlink_status_t status;
      for (int i = 0; i < len; ++i)
      {
        if (mavlink_parse_char(MAVLINK_COMM_0, _buf[i], &msg, &status))
        {
          // have a message, handle it
          handle_message(&msg);
        }
      }
    }
  }
}

void GazeboMavlinkInterface::handle_message(mavlink_message_t *msg)
{
  switch(msg->msgid) {
  case MAVLINK_MSG_ID_HIL_CONTROLS:
    mavlink_hil_controls_t controls;
    mavlink_msg_hil_controls_decode(msg, &controls);
    inputs.control[0] =(double)controls.roll_ailerons;
    inputs.control[1] =(double)controls.pitch_elevator;
    inputs.control[2] =(double)controls.yaw_rudder;
    inputs.control[3] =(double)controls.throttle;
    inputs.control[4] =(double)controls.aux1;
    inputs.control[5] =(double)controls.aux2;
    inputs.control[6] =(double)controls.aux3;
    inputs.control[7] =(double)controls.aux4;

    // publish message
    double scaling = 150;
    double offset = 600;

    input_reference_.resize(_rotor_count);

    for (int i = 0; i < _rotor_count; i++) {
      input_reference_[i] = inputs.control[i] * scaling + offset;
    }

    received_first_referenc_ = true;
    break;
  }
}


GZ_REGISTER_MODEL_PLUGIN(GazeboMavlinkInterface);
}
