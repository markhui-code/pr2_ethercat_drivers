/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2008, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

#include <iomanip>

#include <math.h>
#include <stddef.h>

#include <ethercat_hardware/wg0x.h>

#include <dll/ethercat_dll.h>
#include <al/ethercat_AL.h>
#include <dll/ethercat_device_addressed_telegram.h>
#include <dll/ethercat_frame.h>

#include <boost/crc.hpp>
#include <boost/static_assert.hpp>
#include <boost/make_shared.hpp>

// Temporary,, need 'log' fuction that can switch between fprintf and ROS_LOG.
#define ERR_MODE "\033[41m"
#define STD_MODE "\033[0m"
#define WARN_MODE "\033[43m"
#define GOOD_MODE "\033[42m"
#define INFO_MODE "\033[44m"

#define ERROR_HDR "\033[41mERROR\033[0m"
#define WARN_HDR "\033[43mERROR\033[0m"


unsigned int WG0X::rotateRight8(unsigned in)
{
  in &= 0xff;
  in = (in >> 1) | (in << 7);
  in &= 0xff;
  return in;
}

unsigned WG0X::computeChecksum(void const *data, unsigned length)
{
  const unsigned char *d = (const unsigned char *)data;
  unsigned int checksum = 0x42;
  for (unsigned int i = 0; i < length; ++i)
  {
    checksum = rotateRight8(checksum);
    checksum ^= d[i];
    checksum &= 0xff;
  }
  return checksum;
}

bool WG0XMbxHdr::build(unsigned address, unsigned length, MbxCmdType type, unsigned seqnum)
{
  if (type==LOCAL_BUS_WRITE) 
  {
    if (length > MBX_DATA_SIZE) 
    {
      fprintf(stderr, "size of %d is too large for write\n", length);
      return false;
    }
  }
  else if (type==LOCAL_BUS_READ) 
  {
    // Result of mailbox read, only stores result data + 1byte checksum
    if (length > (MBX_SIZE-1))
    {
      fprintf(stderr, "size of %d is too large for read\n", length);
      return false;      
    }
  }
  else {
    assert(0 && "invalid MbxCmdType");
    return false;
  }
  
  address_ = address;
  length_ = length - 1;
  seqnum_ = seqnum;
  write_nread_ = (type==LOCAL_BUS_WRITE) ? 1 : 0;
  checksum_ = WG0X::rotateRight8(WG0X::computeChecksum(this, sizeof(*this) - 1));
  return true;
}

bool WG0XMbxHdr::verifyChecksum(void) const
{
  return WG0X::computeChecksum(this, sizeof(*this)) != 0;
}

bool WG0XMbxCmd::build(unsigned address, unsigned length, MbxCmdType type, unsigned seqnum, void const* data)
{
  if (!this->hdr_.build(address, length, type, seqnum))
  {
    return false;
  }
      
  if (data != NULL)
  {
    memcpy(data_, data, length);
  }
  else
  {
    memset(data_, 0, length);
  }
  unsigned int checksum = WG0X::rotateRight8(WG0X::computeChecksum(data_, length));
  data_[length] = checksum;
  return true;
}


MbxDiagnostics::MbxDiagnostics() :
  write_errors_(0),
  read_errors_(0),
  lock_errors_(0),
  retries_(0),
  retry_errors_(0)
{
  // Empty
}

WG0XDiagnostics::WG0XDiagnostics() :
  first_(true),
  valid_(false),
  safety_disable_total_(0),
  undervoltage_total_(0),
  over_current_total_(0),
  board_over_temp_total_(0),
  bridge_over_temp_total_(0),
  operate_disable_total_(0),
  watchdog_disable_total_(0),
  lock_errors_(0),
  checksum_errors_(0),
  zero_offset_(0),
  cached_zero_offset_(0)
{
  memset(&safety_disable_status_, 0, sizeof(safety_disable_status_));
  memset(&diagnostics_info_, 0, sizeof(diagnostics_info_));
}

/*!
 * \brief  Use new updates WG0X diagnostics with new safety disable data
 *
 * \param new_status    newly collected safety disable status 
 * \param new_counters  newly collected safety disable counters
 */
void WG0XDiagnostics::update(const WG0XSafetyDisableStatus &new_status, const WG0XDiagnosticsInfo &new_diagnostics_info)
{
  first_ = false;
  safety_disable_total_   += 0xFF & ((uint32_t)(new_status.safety_disable_count_ - safety_disable_status_.safety_disable_count_));
  {
    const WG0XSafetyDisableCounters &new_counters(new_diagnostics_info.safety_disable_counters_);
    const WG0XSafetyDisableCounters &old_counters(diagnostics_info_.safety_disable_counters_);
    undervoltage_total_     += 0xFF & ((uint32_t)(new_counters.undervoltage_count_ - old_counters.undervoltage_count_));
    over_current_total_     += 0xFF & ((uint32_t)(new_counters.over_current_count_ - old_counters.over_current_count_));
    board_over_temp_total_  += 0xFF & ((uint32_t)(new_counters.board_over_temp_count_ - old_counters.board_over_temp_count_));
    bridge_over_temp_total_ += 0xFF & ((uint32_t)(new_counters.bridge_over_temp_count_ - old_counters.bridge_over_temp_count_));
    operate_disable_total_  += 0xFF & ((uint32_t)(new_counters.operate_disable_count_ - old_counters.operate_disable_count_));
    watchdog_disable_total_ += 0xFF & ((uint32_t)(new_counters.watchdog_disable_count_ - old_counters.watchdog_disable_count_));
  } 

  safety_disable_status_   = new_status;
  diagnostics_info_        = new_diagnostics_info;
}


/*!
 * \brief  Verify CRC stored in actuator info structure 
 *
 * ActuatorInfo now constains two CRCs.
 * Originally all devices had EEPROMS with 264 byte pages, and only crc264 was used.  
 * However, support was need for EEPROM with 246 byte pages.
 * To have backwards compatible support, there is also a CRC of first 252 (256-4) bytes.
 *
 * Devices configure in past will only have 264 byte EEPROM pages, and 264byte CRC.  
 * Newer devices might have 256 or 264 byte pages.  
 * The 264 byte EEPROMs will store both CRCs.  
 * The 256 byte EEPROMs will only store the 256 byte CRC.
 * 
 * Thus:
 *  - Old software will be able to use 264 byte EEPROM with new dual CRC.
 *  - New software will be able to use 264 byte EEPROM with single 264 byte CRC
 *  - Only new sofware will be able to use 256 byte EEPROM
 *
 * \param com       EtherCAT communication class used for communicating with device
 * \return          true if CRC is good, false if CRC is invalid
 */
bool WG0XActuatorInfo::verifyCRC() const
{
  // Actuator info contains two 
  BOOST_STATIC_ASSERT(sizeof(WG0XActuatorInfo) == 264);
  BOOST_STATIC_ASSERT( offsetof(WG0XActuatorInfo, crc32_256_) == (256-4));
  BOOST_STATIC_ASSERT( offsetof(WG0XActuatorInfo, crc32_264_) == (264-4));
  boost::crc_32_type crc32_256, crc32_264;  
  crc32_256.process_bytes(this, offsetof(WG0XActuatorInfo, crc32_256_));
  crc32_264.process_bytes(this, offsetof(WG0XActuatorInfo, crc32_264_));
  return ((this->crc32_264_ == crc32_264.checksum()) || (this->crc32_256_ == crc32_256.checksum()));
}

/*!
 * \brief  Calculate CRC of structure and update crc32_256_ and crc32_264_ elements
 */
void WG0XActuatorInfo::generateCRC(void)
{
  boost::crc_32_type crc32;
  crc32.process_bytes(this, offsetof(WG0XActuatorInfo, crc32_256_));
  this->crc32_256_ = crc32.checksum();
  crc32.reset();
  crc32.process_bytes(this, offsetof(WG0XActuatorInfo, crc32_264_));
  this->crc32_264_ = crc32.checksum();
}


WG0X::WG0X() :
  max_current_(0.0),
  too_many_dropped_packets_(false),
  status_checksum_error_(false),
  timestamp_jump_detected_(false),
  fpga_internal_reset_detected_(false),
  cached_zero_offset_(0), 
  calibration_status_(NO_CALIBRATION),
  last_num_encoder_errors_(0),
  app_ram_status_(APP_RAM_MISSING),
  motor_model_(NULL),
  disable_motor_model_checking_(false)
{

  last_timestamp_ = 0;
  last_last_timestamp_ = 0;
  drops_ = 0;
  consecutive_drops_ = 0;
  max_consecutive_drops_ = 0;
  max_board_temperature_ = 0;
  max_bridge_temperature_ = 0;
  in_lockout_ = false;
  resetting_ = false;
  has_error_ = false;

  int error;
  if ((error = pthread_mutex_init(&wg0x_diagnostics_lock_, NULL)) != 0)
  {
    ROS_ERROR("WG0X : init diagnostics mutex :%s", strerror(error));
  }
  if ((error = pthread_mutex_init(&mailbox_lock_, NULL)) != 0)
  {
    ROS_ERROR("WG0X : init mailbox mutex :%s", strerror(error));
  }
}

WG0X::~WG0X()
{
  delete sh_->get_fmmu_config();
  delete sh_->get_pd_config();
  delete motor_model_;
}


void WG0X::construct(EtherCAT_SlaveHandler *sh, int &start_address)
{
  EthercatDevice::construct(sh, start_address);

  // WG EtherCAT devices (WG05,WG06,WG21) revisioning scheme
  fw_major_ = (sh->get_revision() >> 8) & 0xff;
  fw_minor_ = sh->get_revision() & 0xff;
  board_major_ = ((sh->get_revision() >> 24) & 0xff) - 1;
  board_minor_ = (sh->get_revision() >> 16) & 0xff;

  // Would normally configure EtherCAT initialize EtherCAT communication settings here.
  // However, since all WG devices are slightly different doesn't make sense to do it here.
  // Instead make sub-classes handle this.
}


/**  \brief Fills in ethercat_hardware::ActuatorInfo from WG0XActuatorInfo
 *
 * WG0XAcuatorInfo is a packed structure that comes directly from the device EEPROM.
 * ethercat_hardware::ActuatorInfo is a ROS message type that is used by both
 * motor model and motor heating model. 
 */
void WG0X::copyActuatorInfo(ethercat_hardware::ActuatorInfo &out,  const WG0XActuatorInfo &in)
{
  out.id   = in.id_;
  out.name = std::string(in.name_);
  out.robot_name = in.robot_name_;
  out.motor_make = in.motor_make_;
  out.motor_model = in.motor_model_;
  out.max_current = in.max_current_;
  out.speed_constant = in.speed_constant_; 
  out.motor_resistance  = in.resistance_;
  out.motor_torque_constant = in.motor_torque_constant_;
  out.encoder_reduction = in.encoder_reduction_;
  out.pulses_per_revolution = in.pulses_per_revolution_;  
}


/**  \brief Allocates and initialized motor trace for WG0X devices than use it (WG006, WG005)
 */
bool WG0X::initializeMotorModel(pr2_hardware_interface::HardwareInterface *hw, 
                                const string &device_description,
                                double max_pwm_ratio, 
                                double board_resistance,
                                bool   poor_measured_motor_voltage)
{
  if (!hw) 
    return true;

  motor_model_ = new MotorModel(1000);
  if (motor_model_ == NULL) 
    return false;

  const ethercat_hardware::ActuatorInfo &ai(actuator_info_msg_);
  
  unsigned product_code = sh_->get_product_code();
  ethercat_hardware::BoardInfo bi;
  bi.description = device_description; 
  bi.product_code = product_code;
  bi.pcb = board_major_;
  bi.pca = board_minor_;
  bi.serial = sh_->get_serial();
  bi.firmware_major = fw_major_;
  bi.firmware_minor = fw_minor_;
  bi.board_resistance = board_resistance;
  bi.max_pwm_ratio    = max_pwm_ratio;
  bi.hw_max_current   = config_info_.absolute_current_limit_ * config_info_.nominal_current_scale_;
  bi.poor_measured_motor_voltage = poor_measured_motor_voltage;

  if (!motor_model_->initialize(ai,bi))
    return false;
  
  // Create digital out that can be used to force trigger of motor trace
  publish_motor_trace_.name_ = string(actuator_info_.name_) + "_publish_motor_trace";
  publish_motor_trace_.command_.data_ = 0;
  publish_motor_trace_.state_.data_ = 0;
  if (!hw->addDigitalOut(&publish_motor_trace_)) {
    ROS_FATAL("A digital out of the name '%s' already exists", publish_motor_trace_.name_.c_str());
    return false;
  }

  // When working with experimental setups we don't want motor model to halt motors when it detects a problem.
  // Allow rosparam to disable motor model halting for a specific motor.
  if (!ros::NodeHandle().getParam(ai.name + "/disable_motor_model_checking", disable_motor_model_checking_))
  {
    disable_motor_model_checking_ = false;
  }

  return true;
}


boost::shared_ptr<ethercat_hardware::MotorHeatingModelCommon> WG0X::motor_heating_model_common_;

bool WG0X::initializeMotorHeatingModel(bool allow_unprogrammed)
{

  EthercatDirectCom com(EtherCAT_DataLinkLayer::instance());
  ethercat_hardware::MotorHeatingModelParametersEepromConfig config;
  if (!readMotorHeatingModelParametersFromEeprom(&com, config))
  {
    ROS_FATAL("Unable to read motor heating model config parameters from EEPROM");
    return false;
  }

  // All devices need to have motor model heating model parameters stored in them...
  // Even if device doesn't use paramers, they should be there.
  if (!config.verifyCRC())
  {
    if (allow_unprogrammed)
    {
      ROS_WARN("%s EEPROM does not contain motor heating model parameters",
               actuator_info_.name_);
      return true;
    }
    else 
    {
      ROS_WARN("%s EEPROM does not contain motor heating model parameters",
               actuator_info_.name_);
      return true;
      // TODO: once there is ability to update all MCB iwth motorconf, this is will become a fatal error
      ROS_FATAL("%s EEPROM does not contain motor heating model parameters", 
                actuator_info_.name_);
      return false;
    }
  }

  // Even though all devices should contain motor heating model parameters,
  // The heating model does not need to be used.
  if (config.enforce_ == 0)
  {
    return true;
  }

  // Don't need motor model if we are not using ROS (motorconf)
  if (!use_ros_)
  {
    return true;
  }

  // Generate hwid for motor model
  std::ostringstream hwid;
  hwid << unsigned(sh_->get_product_code()) << std::setw(5) << std::setfill('0') << unsigned(sh_->get_serial());

  // All motor heating models use shared settings structure
  if (motor_heating_model_common_.get() == NULL)
  {
    ros::NodeHandle nh("~motor_heating_model");
    motor_heating_model_common_ = boost::make_shared<ethercat_hardware::MotorHeatingModelCommon>(nh);
    motor_heating_model_common_->initialize();
  }

  motor_heating_model_ = 
    boost::make_shared<ethercat_hardware::MotorHeatingModel>(config.params_, 
                                                             actuator_info_.name_, 
                                                             hwid.str(),
                                                             motor_heating_model_common_->save_directory_); 
  // have motor heating model load last saved temperaures from filesystem
  if (motor_heating_model_common_->load_save_files_)
  {
    if (!motor_heating_model_->loadTemperatureState())
    {
      ROS_WARN("Could not load motor temperature state for %s", actuator_info_.name_);
    }
  }
  motor_heating_model_->initialize();
  motor_heating_model_common_->attach(motor_heating_model_);

  return true;
}


int WG0X::initialize(pr2_hardware_interface::HardwareInterface *hw, bool allow_unprogrammed)
{
  ROS_DEBUG("Device #%02d: WG0%d (%#08x) Firmware Revision %d.%02d, PCB Revision %c.%02d, Serial #: %d", 
            sh_->get_ring_position(),
            sh_->get_product_code() % 100,
            sh_->get_product_code(), fw_major_, fw_minor_,
            'A' + board_major_, board_minor_,
            sh_->get_serial());

  EthercatDirectCom com(EtherCAT_DataLinkLayer::instance());

  if (sh_->get_product_code() == WG05_PRODUCT_CODE)
  {
    if (fw_major_ != 1 || fw_minor_ < 7)
    {
      ROS_FATAL("Unsupported firmware revision %d.%02d", fw_major_, fw_minor_);
      return -1;
    }
  }
  else
  {
    if ((fw_major_ == 0 && fw_minor_ < 4) /*|| (fw_major_ == 1 && fw_minor_ < 0)*/)
    {
      ROS_FATAL("Unsupported firmware revision %d.%02d", fw_major_, fw_minor_);
      return -1;
    }
  }

  if (readMailbox(&com, WG0XConfigInfo::CONFIG_INFO_BASE_ADDR, &config_info_, sizeof(config_info_)) != 0)
  {
    ROS_FATAL("Unable to load configuration information");
    return -1;
  }
  ROS_DEBUG("            Serial #: %05d", config_info_.device_serial_number_);
  double board_max_current = double(config_info_.absolute_current_limit_) * config_info_.nominal_current_scale_;

  if (!readActuatorInfoFromEeprom(&com, actuator_info_))
  {
    ROS_FATAL("Unable to read actuator info from EEPROM");
    return -1;
  }
  
  if (actuator_info_.verifyCRC())
  {
    if (actuator_info_.major_ != 0 || actuator_info_.minor_ != 2)
    {
      if (allow_unprogrammed)
        ROS_WARN("Unsupported actuator info version (%d.%d != 0.2).  Please reprogram device #%02d", actuator_info_.major_, actuator_info_.minor_, sh_->get_ring_position());
      else
      {
        ROS_FATAL("Unsupported actuator info version (%d.%d != 0.2).  Please reprogram device #%02d", actuator_info_.major_, actuator_info_.minor_, sh_->get_ring_position());
        return -1;
      }
    }

    actuator_.name_ = actuator_info_.name_;
    ROS_DEBUG("            Name: %s", actuator_info_.name_);

    // Copy actuator info read from eeprom, into msg type
    copyActuatorInfo(actuator_info_msg_, actuator_info_);

    if (!initializeMotorHeatingModel(allow_unprogrammed))
    {
      return -1;
    }


    bool isWG021 = sh_->get_product_code() == WG021_PRODUCT_CODE;
    if (!isWG021)
    {
      // Register actuator with pr2_hardware_interface::HardwareInterface
      if (hw && !hw->addActuator(&actuator_))
      {
          ROS_FATAL("An actuator of the name '%s' already exists.  Device #%02d has a duplicate name", actuator_.name_.c_str(), sh_->get_ring_position());
          return -1;
      }

    }

    // Register digital out with pr2_hardware_interface::HardwareInterface
    digital_out_.name_ = actuator_info_.name_;
    if (hw && !hw->addDigitalOut(&digital_out_))
    {
        ROS_FATAL("A digital out of the name '%s' already exists.  Device #%02d has a duplicate name", digital_out_.name_.c_str(), sh_->get_ring_position());
        return -1;
    }

    // If it is supported, read application ram data.
    if (app_ram_status_ == APP_RAM_PRESENT)
    {
      double zero_offset;
      if (readAppRam(&com, zero_offset))
      {
        ROS_DEBUG("Read calibration from device %s: %f", actuator_info_.name_, zero_offset);
        actuator_.state_.zero_offset_ = zero_offset;
        cached_zero_offset_ = zero_offset;
        calibration_status_ = SAVED_CALIBRATION;
      }
      else
      {
        ROS_DEBUG("No calibration offset was stored on device %s", actuator_info_.name_);
      }
    }
    else if (app_ram_status_ == APP_RAM_MISSING)
    {
      ROS_WARN("Device %s does not support storing calibration offsets", actuator_info_.name_);
    }
    else if (app_ram_status_ == APP_RAM_NOT_APPLICABLE)
    {
      // don't produce warning
    }

    // Make sure motor current limit is less than board current limit
    if (actuator_info_.max_current_ > board_max_current)
    {
      ROS_WARN("WARNING: Device #%02d : motor current limit (%f) greater than board current limit (%f)", 
               sh_->get_ring_position(), actuator_info_.max_current_, board_max_current);
    }
    max_current_ = std::min(board_max_current, actuator_info_.max_current_);
  }
  else if (allow_unprogrammed)
  {
    ROS_WARN("WARNING: Device #%02d (%d%05d) is not programmed", 
             sh_->get_ring_position(), sh_->get_product_code(), sh_->get_serial());
    //actuator_info_.crc32_264_ = 0;
    //actuator_info_.crc32_256_ = 0;

    max_current_ = board_max_current;
  }
  else
  {
    ROS_FATAL("Device #%02d (%d%05d) is not programmed, aborting...", 
              sh_->get_ring_position(), sh_->get_product_code(), sh_->get_serial());
    return -1;
  }

  return 0;
}

#define GET_ATTR(a) \
{ \
  TiXmlElement *c; \
  attr = elt->Attribute((a)); \
  if (!attr) { \
    c = elt->FirstChildElement((a)); \
    if (!c || !(attr = c->GetText())) { \
      ROS_FATAL("Actuator is missing the attribute "#a); \
      exit(EXIT_FAILURE); \
    } \
  } \
}

void WG0X::clearErrorFlags(void)
{
  has_error_ = false;
  too_many_dropped_packets_ = false;
  status_checksum_error_ = false;
  timestamp_jump_detected_ = false;
  if (motor_model_) motor_model_->reset();
  if (motor_heating_model_.get() != NULL) 
  {
    motor_heating_model_->reset();
  }
}

void WG0X::packCommand(unsigned char *buffer, bool halt, bool reset)
{
  pr2_hardware_interface::ActuatorCommand &cmd = actuator_.command_;
  
  if (halt) 
  {
    cmd.effort_ = 0;
  }

  if (reset) 
  {
    clearErrorFlags();
  }
  resetting_ = reset;

  // If zero_offset was changed, give it to non-realtime thread
  double zero_offset = actuator_.state_.zero_offset_;
  if (zero_offset != cached_zero_offset_) 
  {
    if (tryLockWG0XDiagnostics())
    {
      ROS_DEBUG("Calibration change of %s, new %f, old %f", actuator_info_.name_, zero_offset, cached_zero_offset_);
      cached_zero_offset_ = zero_offset;
      wg0x_collect_diagnostics_.zero_offset_ = zero_offset;
      calibration_status_ = CONTROLLER_CALIBRATION;
      unlockWG0XDiagnostics();
    }
    else 
    {
      // It is OK if trylock failed, this will still try again next cycle.
    }
  }

  // Compute the current
  double current = (cmd.effort_ / actuator_info_.encoder_reduction_) / actuator_info_.motor_torque_constant_ ;
  actuator_.state_.last_commanded_effort_ = cmd.effort_;
  actuator_.state_.last_commanded_current_ = current;

  // Truncate the current to limit
  current = max(min(current, max_current_), -max_current_);

  // Pack command structures into EtherCAT buffer
  WG0XCommand *c = (WG0XCommand *)buffer;
  memset(c, 0, command_size_);
  c->programmed_current_ = int(current / config_info_.nominal_current_scale_);
  c->mode_ = (cmd.enable_ && !halt && !has_error_) ? (MODE_ENABLE | MODE_CURRENT) : MODE_OFF;
  c->mode_ |= (reset ? MODE_SAFETY_RESET : 0);
  c->digital_out_ = digital_out_.command_.data_;
  c->checksum_ = rotateRight8(computeChecksum(c, command_size_ - 1));
}

bool WG0X::unpackState(unsigned char *this_buffer, unsigned char *prev_buffer)
{
  pr2_hardware_interface::ActuatorState &state = actuator_.state_;
  WG0XStatus *this_status, *prev_status;

  this_status = (WG0XStatus *)(this_buffer + command_size_);
  prev_status = (WG0XStatus *)(prev_buffer + command_size_);

  digital_out_.state_.data_ = this_status->digital_out_;

  // Do not report timestamp directly to controllers because 32bit integer 
  // value in microseconds will overflow every 72 minutes.   
  // Instead a accumulate small time differences into a ros::Duration variable
  int32_t timediff = WG0X::timestampDiff(this_status->timestamp_, prev_status->timestamp_);
  sample_timestamp_ += WG0X::timediffToDuration(timediff);
  state.sample_timestamp_ = sample_timestamp_;   //ros::Duration is preferred source of time for controllers
  state.timestamp_ = sample_timestamp_.toSec();  //double value is for backwards compatibility
  
  state.device_id_ = sh_->get_ring_position();
  
  state.encoder_count_ = this_status->encoder_count_;
  state.position_ = double(this_status->encoder_count_) / actuator_info_.pulses_per_revolution_ * 2 * M_PI - state.zero_offset_;
  
  state.encoder_velocity_ = 
    calcEncoderVelocity(this_status->encoder_count_, this_status->timestamp_,
                        prev_status->encoder_count_, prev_status->timestamp_);
  state.velocity_ = state.encoder_velocity_ / actuator_info_.pulses_per_revolution_ * 2 * M_PI;

  state.calibration_reading_ = this_status->calibration_reading_ & LIMIT_SENSOR_0_STATE;
  state.calibration_rising_edge_valid_ = this_status->calibration_reading_ &  LIMIT_OFF_TO_ON;
  state.calibration_falling_edge_valid_ = this_status->calibration_reading_ &  LIMIT_ON_TO_OFF;
  state.last_calibration_rising_edge_ = double(this_status->last_calibration_rising_edge_) / actuator_info_.pulses_per_revolution_ * 2 * M_PI;
  state.last_calibration_falling_edge_ = double(this_status->last_calibration_falling_edge_) / actuator_info_.pulses_per_revolution_ * 2 * M_PI;
  state.is_enabled_ = bool(this_status->mode_ & MODE_ENABLE);

  state.last_executed_current_ = this_status->programmed_current_ * config_info_.nominal_current_scale_;
  state.last_measured_current_ = this_status->measured_current_ * config_info_.nominal_current_scale_;

  state.last_executed_effort_ = this_status->programmed_current_ * config_info_.nominal_current_scale_ * actuator_info_.motor_torque_constant_ * actuator_info_.encoder_reduction_;
  state.last_measured_effort_ = this_status->measured_current_ * config_info_.nominal_current_scale_ * actuator_info_.motor_torque_constant_ * actuator_info_.encoder_reduction_;

  state.num_encoder_errors_ = this_status->num_encoder_errors_;

  state.motor_voltage_ = this_status->motor_voltage_ * config_info_.nominal_voltage_scale_;

  state.max_effort_ = max_current_ * actuator_info_.encoder_reduction_ * actuator_info_.motor_torque_constant_; 

  return verifyState(this_status, prev_status);
}


bool WG0X::verifyChecksum(const void* buffer, unsigned size)
{
  bool success = computeChecksum(buffer, size) == 0;
  if (!success) {
    if (tryLockWG0XDiagnostics()) {
      ++wg0x_collect_diagnostics_.checksum_errors_;
      unlockWG0XDiagnostics();
    }
  }
  return success;
}



/**
 * Returns (new_timestamp - old_timestamp).  Accounts for wrapping of timestamp values.
 *
 * It is assumed that each timestamps is exactly 32bit and can wrap around from 0xFFFFFFFF back to 0. 
 * In this case  1 - 4294967295 should equal 2 not -4294967294.  (Note : 0xFFFFFFFF = 4294967295)
 */
int32_t WG0X::timestampDiff(uint32_t new_timestamp, uint32_t old_timestamp)
{
  return int32_t(new_timestamp - old_timestamp);
}

/**
 * Convert timestamp difference to ros::Duration.  Timestamp is assumed to be in microseconds
 *
 * It is assumed that each timestamps is exactly 32bit and can wrap around from 0xFFFFFFFF back to 0. 
 * In this case  1 - 4294967295 should equal 2 not -4294967294.  (Note : 0xFFFFFFFF = 4294967295)
 */
ros::Duration WG0X::timediffToDuration(int32_t timediff_usec)
{
  static const int USEC_PER_SEC = 1000000;
  int sec  = timediff_usec / USEC_PER_SEC;
  int nsec = (timediff_usec % USEC_PER_SEC)*1000;
  return ros::Duration(sec,nsec);
}


/**
 * Returns (new_position - old_position).  Accounts for wrap-around of 32-bit position values.
 *
 * It is assumed that each position value is exactly 32bit and can wrap from -2147483648 to +2147483647.
 */
int32_t WG0X::positionDiff(int32_t new_position, int32_t old_position)
{
  return int32_t(new_position - old_position);
}

/**
 * Returns velocity in encoder ticks per second.
 *
 * Timestamp assumed to be in microseconds
 * Accounts for wrapping of timestamp values and position values.
 */
double WG0X::calcEncoderVelocity(int32_t new_position, uint32_t new_timestamp, 
                                 int32_t old_position, uint32_t old_timestamp)
{
  double timestamp_diff = double(timestampDiff(new_timestamp, old_timestamp)) * 1e-6;
  double position_diff = double(positionDiff(new_position, old_position));
  return (position_diff / timestamp_diff);
}


/**
 * \brief  Converts raw 16bit temperature value returned by device into value in degress Celcius
 * 
 * \param raw_temp  Raw 16bit temperature value return by device
 * \return          Temperature in degrees Celcius
 */
double WG0X::convertRawTemperature(int16_t raw_temp)
{
  return 0.0078125 * double(raw_temp);
}


// Returns true if timestamp changed by more than (amount) or time goes in reverse.
bool WG0X::timestamp_jump(uint32_t timestamp, uint32_t last_timestamp, uint32_t amount)
{
  uint32_t timestamp_diff = (timestamp - last_timestamp);
  return (timestamp_diff > amount);
}

bool WG0X::verifyState(WG0XStatus *this_status, WG0XStatus *prev_status)
{
  pr2_hardware_interface::ActuatorState &state = actuator_.state_;
  bool rv = true;

  if ((motor_model_ != NULL) || (motor_heating_model_ != NULL))
  {
    // Both motor model and motor heating model use MotorTraceSample
    ethercat_hardware::MotorTraceSample &s(motor_trace_sample_);
    double last_executed_current =  this_status->programmed_current_ * config_info_.nominal_current_scale_;
    double supply_voltage = double(prev_status->supply_voltage_) * config_info_.nominal_voltage_scale_;
    double pwm_ratio = double(this_status->programmed_pwm_value_) / double(PWM_MAX);
    s.timestamp        = state.timestamp_;
    s.enabled          = state.is_enabled_;
    s.supply_voltage   = supply_voltage;
    s.measured_motor_voltage = state.motor_voltage_;
    s.programmed_pwm   = pwm_ratio;
    s.executed_current = last_executed_current;
    s.measured_current = state.last_measured_current_;
    s.velocity         = state.velocity_;
    s.encoder_position = state.position_;
    s.encoder_error_count = state.num_encoder_errors_;

    if (motor_model_ != NULL)
    {
      // Collect data for motor model
      motor_model_->sample(s);
      motor_model_->checkPublish();
    }
    if (motor_heating_model_ != NULL)
    {
      double ambient_temperature = convertRawTemperature(this_status->board_temperature_);
      double duration = double(timestampDiff(this_status->timestamp_, prev_status->timestamp_)) * 1e-6;
      motor_heating_model_->update(s, actuator_info_msg_, ambient_temperature, duration);

      if ((!motor_heating_model_common_->disable_halt_) && (motor_heating_model_->hasOverheated()))
      {
        rv = false;
      }
    }
  }

  max_board_temperature_ = max(max_board_temperature_, this_status->board_temperature_);
  max_bridge_temperature_ = max(max_bridge_temperature_, this_status->bridge_temperature_);

  if (this_status->timestamp_ == last_timestamp_ ||
      this_status->timestamp_ == last_last_timestamp_) {
    ++drops_;
    ++consecutive_drops_;
    max_consecutive_drops_ = max(max_consecutive_drops_, consecutive_drops_);
  } else {
    consecutive_drops_ = 0;
  }
  // Detect timestamps going in reverse or changing by more than 10 seconds = 10,000,000 usec
  if ( timestamp_jump(this_status->timestamp_,last_timestamp_,10000000) )
  {
    timestamp_jump_detected_ = true;
  }
  last_last_timestamp_ = last_timestamp_;
  last_timestamp_ = this_status->timestamp_;

  if (consecutive_drops_ > 10)
  {
    too_many_dropped_packets_ = true;
    rv = false;
    goto end;
  }

  in_lockout_ = bool(this_status->mode_ & MODE_SAFETY_LOCKOUT);
  if (in_lockout_ && !resetting_)
  {
    rv = false;
    goto end;
  }

  if (fpga_internal_reset_detected_)
  {
    rv = false;
    goto end;
  }

  if (state.is_enabled_ && motor_model_)
  {
    if (!disable_motor_model_checking_)
    {
      if(!motor_model_->verify())
      {
        // Motor model will automatically publish a motor trace when there is an error
        rv = false;
        goto end;
      }
    }
  }

end:
  if (motor_model_) 
  {
    // Publish trace when:
    //  * device goes into safety lockout
    //  * controller request motor trace to be published
    bool new_error = in_lockout_ && !resetting_ && !has_error_;
    if (new_error || publish_motor_trace_.command_.data_)
    {
      const char* reason = (new_error) ? "Safety Lockout" : "Publishing manually triggered";
      int level          = (new_error) ? 2 : 0;
      motor_model_->flagPublish(reason, level , 100);
      publish_motor_trace_.command_.data_ = 0;
    }
  }
  bool is_error = !rv;
  has_error_ = is_error || has_error_;
  actuator_.state_.halted_ = has_error_ || this_status->mode_ == MODE_OFF;
  return rv;
}

bool WG0X::publishTrace(const string &reason, unsigned level, unsigned delay)
{
  if (motor_model_) 
  {
    motor_model_->flagPublish(reason, level, delay);
    return true;
  }
  return false;
}


void WG0X::collectDiagnostics(EthercatCom *com)
{
  //Collect safety disable information through mailbox  
  bool success = false;

  // Have parent collect diagnositcs
  EthercatDevice::collectDiagnostics(com);

  // Send a packet with both a Fixed address read (NPRW) to device to make sure it is present in chain.
  // This avoids wasting time trying to read mailbox of device that it not present on chain.
  {
    EC_Logic *logic = EC_Logic::instance();
    unsigned char buf[1];
    EC_UINT address = 0x0000;
    NPRD_Telegram nprd_telegram(logic->get_idx(),
                                sh_->get_station_address(),
                                address,
                                0 /*working counter*/,
                                sizeof(buf),
                                buf);
    EC_Ethernet_Frame frame(&nprd_telegram);
    if (!com->txandrx_once(&frame)) {
      // packet didn't come back
      goto end;
    }
    if (nprd_telegram.get_wkc() != 1) {
      // packet came back, but device didn't not respond
      goto end;
    }
  }
 
  WG0XSafetyDisableStatus s;
  if (readMailbox(com, s.BASE_ADDR, &s, sizeof(s)) != 0) {
    goto end;
  }
    
  WG0XDiagnosticsInfo di;
  if (readMailbox(com, di.BASE_ADDR, &di, sizeof(di)) != 0) {
    goto end;
  }
  
  { // Try writing zero offset to to WG0X devices that have application ram
    WG0XDiagnostics &dg(wg0x_collect_diagnostics_);

    if ((app_ram_status_ == APP_RAM_PRESENT) && (dg.zero_offset_ != dg.cached_zero_offset_))
    {
      if (writeAppRam(com, dg.zero_offset_)){
	ROS_DEBUG("Writing new calibration to device %s, new %f, old %f", actuator_info_.name_, dg.zero_offset_, dg.cached_zero_offset_);
	dg.cached_zero_offset_ = dg.zero_offset_;
      }
      else{
	ROS_ERROR("Failed to write new calibration to device %s, new %f, old %f", actuator_info_.name_, dg.zero_offset_, dg.cached_zero_offset_);
	// Diagnostics thread will try again next update cycle
      }
    }
  }

  success = true;

 end:
  if (!lockWG0XDiagnostics()) {
    wg0x_collect_diagnostics_.valid_ = false;   // change these values even if we did't get the lock
    wg0x_collect_diagnostics_.first_ = false;   
    return;
  }

  wg0x_collect_diagnostics_.valid_ = success;   
  if (success) {
    wg0x_collect_diagnostics_.update(s,di);
  }

  unlockWG0XDiagnostics();
}


bool WG0X::writeAppRam(EthercatCom *com, double zero_offset) 
{
  WG0XUserConfigRam cfg;
  cfg.version_ = 1;
  cfg.zero_offset_ = zero_offset;
  boost::crc_32_type crc32;
  crc32.process_bytes(&cfg, sizeof(cfg)-sizeof(cfg.crc32_));
  cfg.crc32_ = crc32.checksum();
  return (writeMailbox(com, WG0XUserConfigRam::BASE_ADDR, &cfg, sizeof(cfg)) == 0);
}

bool WG0X::readAppRam(EthercatCom *com, double &zero_offset) 
{
  WG0XUserConfigRam cfg;
  if (!readMailbox(com, WG0XUserConfigRam::BASE_ADDR, &cfg, sizeof(cfg)) == 0)
  {
    return false;
  }
  if (cfg.version_ != 1) 
  {
    return false;
  }
  boost::crc_32_type crc32;
  crc32.process_bytes(&cfg, sizeof(cfg)-sizeof(cfg.crc32_));
  if (cfg.crc32_ != crc32.checksum()) {
    return false;
  }
  zero_offset = cfg.zero_offset_;
  return true;
}


/*!
 * \brief  Waits for SPI eeprom state machine to be idle.
 *
 * Polls busy SPI bit of SPI state machine. 
 * 
 * \param com       EtherCAT communication class used for communicating with device
 * \return          true if state machine is free, false if there is an error, or we timed out waiting
 */
bool WG0X::waitForSpiEepromReady(EthercatCom *com)
{
  WG0XSpiEepromCmd cmd;
  // TODO : poll for a given number of millseconds instead of a given number of cycles
  //start_time = 0;
  unsigned tries = 0;
  do {
    //read_time = time;
    ++tries;
    if (!readSpiEepromCmd(com, cmd))
    {
      ROS_ERROR("Error reading SPI Eeprom Cmd busy bit");
      return false;
    }

    if (!cmd.busy_) 
    {
      return true;
    }       
    
    usleep(100);
  } while (tries <= 10);

  ROS_ERROR("Timed out waiting for SPI state machine to be idle (%d)", tries);
  return false;
}


/*!
 * \brief  Sends command to SPI EEPROM state machine.   
 *
 * This function makes sure SPI EEPROM state machine is idle before sending new command.
 * It also waits for state machine to be idle before returning.
 * 
 * \param com       EtherCAT communication class used for communicating with device
 * \return          true if command was send, false if there is an error
 */
bool WG0X::sendSpiEepromCmd(EthercatCom *com, const WG0XSpiEepromCmd &cmd)
{
  if (!waitForSpiEepromReady(com))
  {
    return false;
  }

  // Send command
  if (writeMailbox(com, WG0XSpiEepromCmd::SPI_COMMAND_ADDR, &cmd, sizeof(cmd)))
  {
    ROS_ERROR("Error writing SPI EEPROM command");
    return false;
  }

  // Now read back SPI EEPROM state machine register, and check : 
  //  1. for state machine to become ready
  //  2. that command data was properly write and not corrupted
  WG0XSpiEepromCmd stat;
  unsigned tries = 0;
  do
  {
    if (!readSpiEepromCmd(com, stat))
    {
      return false;
    }

    if (stat.operation_ != cmd.operation_)
    {
      ROS_ERROR("Invalid readback of SPI EEPROM operation : got 0x%X, expected 0x%X\n", stat.operation_, cmd.operation_);
      return false;
    }

    // return true if command has completed
    if (!stat.busy_)
    {
      if (tries > 0) 
      {
        ROS_WARN("Eeprom state machine took %d cycles", tries);
      }
      return true;;
    }

    fprintf(stderr, "eeprom busy reading again, waiting...\n");
    usleep(100);
  } while (++tries < 10);

  ROS_ERROR("Eeprom SPI state machine busy after %d cycles", tries);
  return false;
}


/*!
 * \brief  Read data from single eeprom page. 
 *
 * Data should be less than 264 bytes.  Note that some eeproms only support 256 byte pages.  
 * If 264 bytes of data are read from a 256 byte eeprom, then last 8 bytes of data will be zeros.
 * 
 * \param com       EtherCAT communication class used for communicating with device
 * \param page      EEPROM page number to read from.  Should be 0 to 4095.
 * \param data      pointer to data buffer
 * \param length    length of data in buffer
 * \return          true if there is success, false if there is an error
 */
bool WG0X::readEepromPage(EthercatCom *com, unsigned page, void* data, unsigned length)
{
  if (length > MAX_EEPROM_PAGE_SIZE)
  {
    ROS_ERROR("Eeprom read length %d > %d", length, MAX_EEPROM_PAGE_SIZE);
    return false;
  }

  if (page >= NUM_EEPROM_PAGES)
  {
    ROS_ERROR("Eeprom read page %d > %d", page, NUM_EEPROM_PAGES-1);
    return false;
  }

  // Since we don't know the size of the eeprom there is not always 264 bytes available.
  // This may try to read 264 bytes, but only the first 256 bytes may be valid.  
  // To avoid any odd issue, zero out FPGA buffer before asking for eeprom data.
  memset(data,0,length);  
  if (writeMailbox(com, WG0XSpiEepromCmd::SPI_BUFFER_ADDR, &actuator_info_, sizeof(actuator_info_))) 
  {
    ROS_ERROR("Error zeroing eeprom data buffer");
    return false;
  }

  // Send command to SPI state machine to perform read of eeprom, 
  // sendSpiEepromCmd will automatically wait for SPI state machine 
  // to be idle before a new command is sent
  WG0XSpiEepromCmd cmd;
  memset(&cmd,0,sizeof(cmd));
  cmd.build_read(page);
  if (!sendSpiEepromCmd(com, cmd)) 
  {
    ROS_ERROR("Error sending SPI read command");
    return false;
  }

  // Wait for SPI Eeprom Read to complete
  // sendSPICommand will wait for Command to finish before returning

  // Read eeprom page data from FPGA buffer
  if (readMailbox(com, WG0XSpiEepromCmd::SPI_BUFFER_ADDR, data, length)) 
  {
    ROS_ERROR("Error reading eeprom data from buffer");
    return false;
  }

  return true;
}


/*!
 * \brief  Reads actuator info from eeprom.  
 * 
 * \param com       EtherCAT communication class used for communicating with device
 * \param acuator_info Structure where actuator info will be stored.
 * \return          true if there is success, false if there is an error
 */
bool WG0X::readActuatorInfoFromEeprom(EthercatCom *com, WG0XActuatorInfo &actuator_info)
{
  BOOST_STATIC_ASSERT(sizeof(actuator_info) == 264);

  if (!readEepromPage(com, ACTUATOR_INFO_PAGE, &actuator_info, sizeof(actuator_info)))
  {
    ROS_ERROR("Reading acutuator info from eeprom");
    return false;
  }
  return true;
}
 
/*!
 * \brief  Reads actuator info from eeprom.  
 * 
 * \param com       EtherCAT communication class used for communicating with device
 * \param acuator_info Structure where actuator info will be stored.
 * \return          true if there is success, false if there is an error
 */
bool WG0X::readMotorHeatingModelParametersFromEeprom(EthercatCom *com, MotorHeatingModelParametersEepromConfig &config)
{
  BOOST_STATIC_ASSERT(sizeof(config) == 256);

  if (!readEepromPage(com, config.EEPROM_PAGE, &config, sizeof(config)))
  {
    ROS_ERROR("Reading motor heating model config from eeprom");
    return false;
  }
  return true;
}



/*!
 * \brief  Write data to single eeprom page. 
 *
 * Data should be less than 264 bytes.  If data size is less than 264 bytes, then 
 * the page will be padded with 0xFF.  Note that some eeproms only support 256 byte
 * pages.  With 256 byte eeproms, the eeprom FW with ingore last 8 bytes of requested write.
 * 
 * \param com       EtherCAT communication class used for communicating with device
 * \param page      EEPROM page number to write to.  Should be 0 to 4095.
 * \param data      pointer to data buffer
 * \param length    length of data in buffer.  If length < 264, eeprom page will be padded out to 264 bytes.
 * \return          true if there is success, false if there is an error
 */
bool WG0X::writeEepromPage(EthercatCom *com, unsigned page, const void* data, unsigned length)
{
  if (length > 264)
  {
    ROS_ERROR("Eeprom write length %d > %d", length, MAX_EEPROM_PAGE_SIZE);
    return false;
  }

  if (page >= NUM_EEPROM_PAGES)
  {
    ROS_ERROR("Eeprom write page %d > %d", page, NUM_EEPROM_PAGES-1);
    return false;
  }

  // wait for eeprom to be ready before write data into FPGA buffer
  if (!waitForSpiEepromReady(com))
  {
    return false;
  }

  const void *write_buf = data;

  // if needed, pad data to 264 byte in buf
  uint8_t buf[MAX_EEPROM_PAGE_SIZE];
  if (length < MAX_EEPROM_PAGE_SIZE)
  {
    memcpy(buf, data, length);
    memset(buf+length, 0xFF, MAX_EEPROM_PAGE_SIZE-length);
    write_buf = buf;    
  }

  // Write data to FPGA buffer
  if (writeMailbox(com, WG0XSpiEepromCmd::SPI_BUFFER_ADDR, write_buf, MAX_EEPROM_PAGE_SIZE))
  {
    ROS_ERROR("Write of SPI EEPROM buffer failed");
    return false;
  }

  // Have SPI EEPROM state machine start SPI data transfer
  WG0XSpiEepromCmd cmd;
  cmd.build_write(page);
  if (!sendSpiEepromCmd(com, cmd)) 
  {
    ROS_ERROR("Error giving SPI EEPROM write command");
    return false;
  }

  // Wait for EEPROM write to complete
  if (!waitForEepromReady(com))
  {
    return false;
  }

  return true;
}


/*!
 * \brief  Waits for EEPROM to become ready
 *
 * Certain eeprom operations (such as page reads), are complete immediately after data is 
 * trasferred.  Other operations (such as page writes) take some amount of time after data
 * is trasfered to complete.  This polls the EEPROM status register until the 'ready' bit 
 * is set.
 * 
 * \param com       EtherCAT communication class used for communicating with device
 * \return          true if there is success, false if there is an error or wait takes too long
 */
bool WG0X::waitForEepromReady(EthercatCom *com)
{
  // Wait for eeprom write to complete
  unsigned tries = 0;
  EepromStatusReg status_reg;
  do {
    if (!readEepromStatusReg(com, status_reg))
    {
      return false;
    }
    if (status_reg.ready_)
    {
      break;
    }
    usleep(100);
  } while (++tries < 20);

  if (!status_reg.ready_) 
  {
    ROS_ERROR("Eeprom still busy after %d cycles", tries);
    return false;
  } 

  if (tries > 10)
  {
    ROS_WARN("EEPROM took %d cycles to be ready", tries);
  }
  return true;
}



/*!
 * \brief  Reads EEPROM status register
 *
 * Amoung other things, eeprom status register provide information about whether eeprom 
 * is busy performing a write.
 * 
 * \param com       EtherCAT communication class used for communicating with device
 * \param reg       reference to EepromStatusReg struct where eeprom status will be stored
 * \return          true if there is success, false if there is an error
 */
bool WG0X::readEepromStatusReg(EthercatCom *com, EepromStatusReg &reg)
{
  // Status is read from EEPROM by having SPI state machine perform an "abitrary" operation.
  // With an arbitrary operation, the SPI state machine shifts out byte from buffer, while
  // storing byte shifted in from device into same location in buffer.
  // SPI state machine has no idea what command it is sending device or how to intpret its result.

  // To read eeprom status register, we transfer 2 bytes.  The first byte is the read status register 
  // command value (0xD7).  When transfering the second byte, the EEPROM should send its status.
  char data[2] = {0xD7, 0x00};
  BOOST_STATIC_ASSERT(sizeof(data) == 2);
  if (writeMailbox(com, WG0XSpiEepromCmd::SPI_BUFFER_ADDR, data, sizeof(data)))
  {
    ROS_ERROR("Writing SPI buffer");
    return false;
  }
    
  { // Have SPI state machine trasfer 2 bytes
    WG0XSpiEepromCmd cmd;
    cmd.build_arbitrary(sizeof(data));
    if (!sendSpiEepromCmd(com, cmd)) 
    {
      ROS_ERROR("Sending SPI abitrary command");
      return false;
    }
  }
    
  // Data read from device should now be stored in FPGA buffer
  if (readMailbox(com, WG0XSpiEepromCmd::SPI_BUFFER_ADDR, data, sizeof(data)))
  {
    ROS_ERROR("Reading status register data from SPI buffer");
    return false;
  }
 
  // Status register would be second byte of buffer
  reg.raw_ = data[1];
  return true;
}


/*!
 * \brief  Reads SPI state machine command register
 *
 * For communicating with EEPROM, there is a simple state machine that transfers
 * data to/from FPGA buffer over SPI.  
 * When any type of comunication is done with EEPROM:
 *  1. Write command or write data into FPGA buffer.
 *  2. Have state machine start transfer bytes from buffer to EEPROM, and write data from EEPROM into buffer
 *  3. Wait for state machine to complete (by reading its status)
 *  4. Read EEPROM response from FPGA buffer.
 * 
 * \param com       EtherCAT communication class used for communicating with device
 * \param reg       reference to WG0XSpiEepromCmd struct where read data will be stored
 * \return          true if there is success, false if there is an error
 */
bool WG0X::readSpiEepromCmd(EthercatCom *com, WG0XSpiEepromCmd &cmd)
{
  BOOST_STATIC_ASSERT(sizeof(WG0XSpiEepromCmd) == 3);
  if (readMailbox(com, WG0XSpiEepromCmd::SPI_COMMAND_ADDR, &cmd, sizeof(cmd)))
  {
    ROS_ERROR("Reading SPI command register with mailbox");
    return false;
  }
  
  return true;
}


/*!
 * \brief  Programs acutator and heating parameters into device EEPROM.
 *
 * WG0X devices store configuaration info in EEPROM.  This configuration information contains
 * information such as device name, motor parameters, and encoder parameters.  
 *
 * Originally, devices only stored ActuatorInfo in EEPROM.
 * However, later we discovered that in extreme cases, certain motors may overheat.  
 * To prevent motor overheating, a model is used to estimate motor winding temperature
 * and stop motor if temperature gets too high.  
 * However, the new motor heating model needs more motor parameters than were originally stored
 * in eeprom. 
 * 
 * \param com       EtherCAT communication class used for communicating with device
 * \param actutor_info  Actuator information to be stored in device EEPROM
 * \param actutor_info  Motor heating motor information to be stored in device EEPROM
 * \return          true if there is success, false if there is an error
 */
bool WG0X::program(EthercatCom *com, const WG0XActuatorInfo &actutor_info)
{
  if (!writeEepromPage(com, ACTUATOR_INFO_PAGE, &actutor_info, sizeof(actutor_info)))
  {
    ROS_ERROR("Writing actuator infomation to EEPROM");
    return false;
  }
  
  return true;
}


/*!
 * \brief  Programs motor heating parameters into device EEPROM.
 *
 * Originally, devices only stored ActuatorInfo in EEPROM.
 * However, later we discovered that in extreme cases, certain motors may overheat.  
 * To prevent motor overheating, a model is used to estimate motor winding temperature
 * and stop motor if temperature gets too high.  
 * However, the new motor heating model needs more motor parameters than were originally stored
 * in eeprom. 
 * 
 * \param com       EtherCAT communication class used for communicating with device
 * \param heating_config  Motor heating model parameters to be stored in device EEPROM
 * \return          true if there is success, false if there is an error
 */
bool WG0X::program(EthercatCom *com, const ethercat_hardware::MotorHeatingModelParametersEepromConfig &heating_config)
{
  if (!writeEepromPage(com, heating_config.EEPROM_PAGE, &heating_config, sizeof(heating_config)))
  {
    ROS_ERROR("Writing motor heating model configuration to EEPROM");
    return false;
  }
  
  return true;  
}

/*!
 * \brief  Find differece between two timespec values
 *
 * \param current   current time 
 * \param current   start time 
 * \return          returns time difference (current-start) in milliseconds
 */
int timediff_ms(const timespec &current, const timespec &start)
{
  int timediff_ms = (current.tv_sec-start.tv_sec)*1000 // 1000 ms in a sec
    + (current.tv_nsec-start.tv_nsec)/1000000; // 1000000 ns in a ms
  return timediff_ms;
}


/*!
 * \brief  error checking wrapper around clock_gettime
 *
 * \param current   current time 
 * \param current   start time 
 * \return          returns 0 for success, non-zero for failure
 */
int safe_clock_gettime(clockid_t clk_id, timespec *time)
{
  int result = clock_gettime(clk_id, time);
  if (result != 0) {
    int error = errno;
    fprintf(stderr, "safe_clock_gettime : %s\n", strerror(error));
    return result;
  }  
  return result;
}


/*!
 * \brief  safe version of usleep.
 *
 * Uses nanosleep internally.  Will restart sleep after begin woken by signal.
 *
 * \param usec   number of microseconds to sleep for.  Must be < 1000000.
 */
void safe_usleep(uint32_t usec) 
{
  assert(usec<1000000);
  if (usec>1000000)
    usec=1000000;
  struct timespec req, rem;
  req.tv_sec = 0;
  req.tv_nsec = usec*1000;
  while (nanosleep(&req, &rem)!=0) { 
    int error = errno;
    fprintf(stderr,"%s : Error : %s\n", __func__, strerror(error));    
    if (error != EINTR) {
      break;
    }
    req = rem;
  }
  return;
}


unsigned SyncMan::baseAddress(unsigned num) 
{
  assert(num < 8);
  return BASE_ADDR + 8 * num;
}  
  

/*!
 * \brief  Read data from Sync Manager
 *
 * \param com       used to perform communication with device
 * \param sh        slave to read data from
 * \param addrMode  addressing mode used to read data (FIXED/POSITIONAL)
 * \param num       syncman number to read 0-7
 * \return          returns true for success, false for failure 
 */
bool SyncMan::readData(EthercatCom *com, EtherCAT_SlaveHandler *sh, EthercatDevice::AddrMode addrMode, unsigned num)
{
  return ( EthercatDevice::readData(com, sh, baseAddress(num), this, sizeof(*this), addrMode) == 0);
}


unsigned SyncManActivate::baseAddress(unsigned num)
{
  assert(num < 8);
  return BASE_ADDR + 8 * num;
}

/*!
 * \brief  Write data to Sync Manager Activation register
 *
 * \param com       used to perform communication with device
 * \param sh        slave to read data from
 * \param addrMode  addressing mode used to read data (FIXED/POSITIONAL)
 * \param num       syncman number to read 0-7
 * \return          returns true for success, false for failure 
 */
bool SyncManActivate::writeData(EthercatCom *com, EtherCAT_SlaveHandler *sh, EthercatDevice::AddrMode addrMode, unsigned num) const
{
  return ( EthercatDevice::writeData(com, sh, baseAddress(num), this, sizeof(*this), addrMode) == 0);
}


void updateIndexAndWkc(EC_Telegram *tg, EC_Logic *logic) 
{
  tg->set_idx(logic->get_idx());
  tg->set_wkc(logic->get_wkc());
}


bool WG0X::verifyDeviceStateForMailboxOperation()
{
  // Make sure slave is in correct state to do use mailbox
  EC_State state = sh_->get_state();
  if ((state != EC_SAFEOP_STATE) && (state != EC_OP_STATE)) {
    fprintf(stderr, "%s : " ERROR_HDR 
            "cannot do mailbox read in current device state = %d\n", __func__, state);
    return false;
  }
  return true;
}


/*!
 * \brief  Runs diagnostic on read and write mailboxes.
 *
 * Collects and data from mailbox control registers.
 *
 * \todo            not implemented yet
 * \param com       used to perform communication with device
 * \return          returns true for success, false for failure 
 */
void WG0X::diagnoseMailboxError(EthercatCom *com)
{
  
}

/*!
 * \brief  Clears read mailbox by reading first and last byte.
 *
 * Mailbox lock should be held when this function is called.
 *
 * \param com       used to perform communication with device
 * \return          returns true for success, false for failure 
 */
bool WG0X::clearReadMailbox(EthercatCom *com)
{
  if (!verifyDeviceStateForMailboxOperation()){
    return false;
  }

  EC_Logic *logic = EC_Logic::instance();    
  EC_UINT station_addr = sh_->get_station_address();  
  
  // Create Ethernet packet with two EtherCAT telegrams inside of it : 
  //  - One telegram to read first byte of mailbox
  //  - One telegram to read last byte of mailbox
  unsigned char unused[1] = {0};
  NPRD_Telegram read_start(
            logic->get_idx(),
            station_addr,
            MBX_STATUS_PHY_ADDR,
            logic->get_wkc(),
            sizeof(unused),
            unused);
  NPRD_Telegram read_end(  
            logic->get_idx(),
            station_addr,
            MBX_STATUS_PHY_ADDR+MBX_STATUS_SIZE-1,
            logic->get_wkc(),
            sizeof(unused),
             unused);
  read_start.attach(&read_end);
  EC_Ethernet_Frame frame(&read_start);


  // Retry sending packet multiple times 
  bool success=false;
  static const unsigned MAX_DROPS = 15;
  for (unsigned tries=0; tries<MAX_DROPS; ++tries) {
    success = com->txandrx_once(&frame);
    if (success) {
      break;
    }
    updateIndexAndWkc(&read_start, logic);
    updateIndexAndWkc(&read_end  , logic);
  }

  if (!success) {
    fprintf(stderr, "%s : " ERROR_HDR 
            " too much packet loss\n", __func__);   
    safe_usleep(100);
    return false;
  }
  
  // Check result for consistancy
  if (read_start.get_wkc() != read_end.get_wkc()) {
    fprintf(stderr, "%s : " ERROR_HDR 
            "read mbx working counters are inconsistant, %d, %d\n",
            __func__, read_start.get_wkc(), read_end.get_wkc());
    return false;
  }
  if (read_start.get_wkc() > 1) {
    fprintf(stderr, "%s : " ERROR_HDR 
            "more than one device (%d) responded \n", __func__, read_start.get_wkc());
    return false;
  }
  if (read_start.get_wkc() == 1)  {
    fprintf(stderr, "%s : " WARN_MODE "WARN" STD_MODE 
            " read mbx contained garbage data\n", __func__);
    // Not an error, just warning
  } 
  
  return true;  
}



/*!
 * \brief  Waits until read mailbox is full or timeout.
 *
 * Wait times out after 100msec.
 * Mailbox lock should be held when this function is called.
 *
 * \param com       used to perform communication with device
 * \return          returns true for success, false for failure or timeout
 */
bool WG0X::waitForReadMailboxReady(EthercatCom *com)
{
  // Wait upto 100ms for device to toggle ack
  static const int MAX_WAIT_TIME_MS = 100;
  int timediff;
  unsigned good_results=0;


  struct timespec start_time, current_time;
  if (safe_clock_gettime(CLOCK_MONOTONIC, &start_time)!=0) {
    return false;
  }
  
  do {      
    // Check if mailbox is full by looking at bit 3 of SyncMan status register.
    uint8_t SyncManStatus=0;
    const unsigned SyncManAddr = 0x805+(MBX_STATUS_SYNCMAN_NUM*8);
    if (readData(com, SyncManAddr, &SyncManStatus, sizeof(SyncManStatus), FIXED_ADDR) == 0) {
      ++good_results;
      const uint8_t MailboxStatusMask = (1<<3);
      if (SyncManStatus & MailboxStatusMask) {
        return true;
      }
    }      
    if (safe_clock_gettime(CLOCK_MONOTONIC, &current_time)!=0) {
      return false;
      }
    timediff = timediff_ms(current_time, start_time);
    safe_usleep(100);
  } while (timediff < MAX_WAIT_TIME_MS);
  
  if (good_results == 0) {
    fprintf(stderr, "%s : " ERROR_HDR 
            " error reading from device\n", __func__);          
  } else {
    fprintf(stderr, "%s : " ERROR_HDR 
            " error read mbx not full after %d ms\n", __func__, timediff);      
  }

  return false;
}


/*!
 * \brief  Waits until write mailbox is empty or timeout.
 *
 * Wait times out after 100msec.
 * Mailbox lock should be held when this function is called.
 *
 * \param com       used to perform communication with device
 * \return          returns true for success, false for failure or timeout
 */
bool WG0X::waitForWriteMailboxReady(EthercatCom *com)
{
  // Wait upto 100ms for device to toggle ack
  static const int MAX_WAIT_TIME_MS = 100;
  int timediff;
  unsigned good_results=0;


  struct timespec start_time, current_time;
  if (safe_clock_gettime(CLOCK_MONOTONIC, &start_time)!=0) {
    return false;
  }
  
  do {      
    // Check if mailbox is full by looking at bit 3 of SyncMan status register.
    uint8_t SyncManStatus=0;
    const unsigned SyncManAddr = 0x805+(MBX_COMMAND_SYNCMAN_NUM*8);
    if (readData(com, SyncManAddr, &SyncManStatus, sizeof(SyncManStatus), FIXED_ADDR) == 0) {
      ++good_results;
      const uint8_t MailboxStatusMask = (1<<3);
      if ( !(SyncManStatus & MailboxStatusMask) ) {
        return true;
      }
    }      
    if (safe_clock_gettime(CLOCK_MONOTONIC, &current_time)!=0) {
      return false;
    }
    timediff = timediff_ms(current_time, start_time);
    safe_usleep(100);
  } while (timediff < MAX_WAIT_TIME_MS);
  
  if (good_results == 0) {
    fprintf(stderr, "%s : " ERROR_HDR 
            " error reading from device\n", __func__);          
  } else {
    fprintf(stderr, "%s : " ERROR_HDR 
            " error write mbx not empty after %d ms\n", __func__, timediff);      
  }

  return false;
}



/*!
 * \brief  Writes data to mailbox.
 *
 * Will try to conserve bandwidth by only length bytes of data and last byte of mailbox.
 * Mailbox lock should be held when this function is called.
 *
 * \param com       used to perform communication with device
 * \param data      pointer to buffer where read data is stored.
 * \param length    amount of data to read from mailbox
 * \return          returns true for success, false for failure
 */
bool WG0X::writeMailboxInternal(EthercatCom *com, void const *data, unsigned length)
{
  if (length > MBX_COMMAND_SIZE) {
    assert(length <= MBX_COMMAND_SIZE);
    return false;
  }

  // Make sure slave is in correct state to use mailbox
  if (!verifyDeviceStateForMailboxOperation()){
    return false;
  }

  EC_Logic *logic = EC_Logic::instance();    
  EC_UINT station_addr = sh_->get_station_address();
  

  // If there enough savings, split mailbox write up into 2 parts : 
  //  1. Write of actual data to begining of mbx buffer
  //  2. Write of last mbx buffer byte, to complete write
  static const unsigned TELEGRAM_OVERHEAD = 50;
  bool split_write = (length+TELEGRAM_OVERHEAD) < MBX_COMMAND_SIZE;
    
  unsigned write_length = MBX_COMMAND_SIZE;
  if (split_write) {
    write_length = length;
  }

  // Possible do multiple things at once...
  //  1. Clear read mailbox by reading both first and last mailbox bytes
  //  2. Write data into write mailbox
  {
    // Build frame with 2-NPRD + 2 NPWR
    unsigned char unused[1] = {0};
    NPWR_Telegram write_start(
                              logic->get_idx(),
                              station_addr,
                              MBX_COMMAND_PHY_ADDR,
                              logic->get_wkc(),
                              write_length,
                              (const unsigned char*) data);
    NPWR_Telegram write_end(
                            logic->get_idx(),
                            station_addr,
                            MBX_COMMAND_PHY_ADDR+MBX_COMMAND_SIZE-1,
                            logic->get_wkc(),
                            sizeof(unused),
                            unused);
      
    if (split_write) {
      write_start.attach(&write_end);
    }      

    EC_Ethernet_Frame frame(&write_start);
      
    // Try multiple times, but remember number of of successful sends
    unsigned sends=0;      
    bool success=false;
    for (unsigned tries=0; (tries<10) && !success; ++tries) {
      success = com->txandrx_once(&frame);
      if (!success) {
        updateIndexAndWkc(&write_start, logic);
        updateIndexAndWkc(&write_end, logic);
      }
      ++sends; //EtherCAT_com d/n support split TX and RX class, assume tx part of txandrx always succeeds
      /* 
      int handle = com->tx(&frame);
      if (handle > 0) {
        ++sends;
        success = com->rx(&frame, handle);
      }
      if (!success) {
        updateIndexAndWkc(&write_start, logic);
        updateIndexAndWkc(&write_end, logic);
      }
      */
    }
    if (!success) {
      fprintf(stderr, "%s : " ERROR_HDR 
              " too much packet loss\n", __func__);   
      safe_usleep(100);
      return false;
    }
      
    if (split_write && (write_start.get_wkc() != write_end.get_wkc())) {
      fprintf(stderr, "%s : " ERROR_HDR 
              " write mbx working counters are inconsistant\n", __func__);
      return false;
    }

    if (write_start.get_wkc() > 1) 
    {
      fprintf(stderr, "%s : " ERROR_HDR
              " multiple (%d) devices responded to mailbox write\n", __func__, write_start.get_wkc());
      return false;
    }
    else if (write_start.get_wkc() != 1)
    {
      // Write to cmd mbx was refused 
      if (sends<=1) {
        // Packet was only sent once, there must be a problem with slave device
        fprintf(stderr, "%s : " ERROR_HDR 
                " initial mailbox write refused\n", __func__);
        safe_usleep(100);
        return false;
      } else {
        // Packet was sent multiple times because a packet drop occured  
        // If packet drop occured on return path from device, a refusal is acceptable
        fprintf(stderr, "%s : " WARN_HDR 
                " repeated mailbox write refused\n", __func__);
      }
    }     
  }

  return true;
}

bool WG0X::readMailboxRepeatRequest(EthercatCom *com)
{
  bool success = _readMailboxRepeatRequest(com);
  ++mailbox_diagnostics_.retries_;
  if (!success) {
    ++mailbox_diagnostics_.retry_errors_;
  }
  return success;
}

bool WG0X::_readMailboxRepeatRequest(EthercatCom *com)
{
  // Toggle repeat request flag, wait for ack from device
  // Returns true if ack is received, false for failure
  SyncMan sm;
  if (!sm.readData(com, sh_, FIXED_ADDR, MBX_STATUS_SYNCMAN_NUM)) {
    fprintf(stderr, "%s : " ERROR_HDR 
            " could not read status mailbox syncman (1)\n", __func__);
    return false;
  }
  
  // If device can handle repeat requests, then request and ack bit should already match
  if (sm.activate.repeat_request != sm.pdi_control.repeat_ack) {
    fprintf(stderr, "%s : " ERROR_HDR 
            " syncman repeat request and ack do not match\n", __func__);
    return false;
  }

  // Write toggled repeat request,,, wait for ack.
  SyncManActivate orig_activate(sm.activate);
  sm.activate.repeat_request = ~orig_activate.repeat_request;
  if (!sm.activate.writeData(com, sh_, FIXED_ADDR, MBX_STATUS_SYNCMAN_NUM)) {
    fprintf(stderr, "%s : " ERROR_HDR 
            " could not write syncman repeat request\n", __func__);
    //ec_mark(sh->getEM(), "could not write syncman repeat request", 1);
    return false;
  }
  
  // Wait upto 100ms for device to toggle ack
  static const int MAX_WAIT_TIME_MS = 100;
  int timediff;

  struct timespec start_time, current_time;
  if (safe_clock_gettime(CLOCK_MONOTONIC, &start_time)!=0) {
    return false;
  }
  
  do {
    if (!sm.readData(com, sh_, FIXED_ADDR, MBX_STATUS_SYNCMAN_NUM)) {
      fprintf(stderr, "%s : " ERROR_HDR 
              " could not read status mailbox syncman (2)\n", __func__);
      return false;
    }

    if (sm.activate.repeat_request == sm.pdi_control.repeat_ack) {
      // Device responded, to some checks to make sure it seems to be telling the truth
      if (sm.status.mailbox_status != 1) {
        fprintf(stderr, "%s : " ERROR_HDR 
                " got repeat response, but read mailbox is still empty\n", __func__);
        //sm.print(WG0X_MBX_Status_Syncman_Num, std::cerr);
        return false;
      }
      return true;
    }
    
    if ( (sm.activate.repeat_request) == (orig_activate.repeat_request) ) {          
      fprintf(stderr, "%s : " ERROR_HDR 
              " syncman repeat request was changed while waiting for response\n", __func__);
      //sm.activate.print();
      //orig_activate.print();
      return false;
    }

    if (safe_clock_gettime(CLOCK_MONOTONIC, &current_time)!=0) {
      return false;
    }
    
    timediff = timediff_ms(current_time, start_time);
    safe_usleep(100);        
  } while (timediff < MAX_WAIT_TIME_MS);
    
  fprintf(stderr, "%s : " ERROR_HDR 
          " error repeat request not acknowledged after %d ms\n", __func__, timediff);    
  return false;
}



/*!
 * \brief  Reads data from read mailbox.
 *
 * Will try to conserve bandwidth by reading length bytes of data and last byte of mailbox.
 * Mailbox lock should be held when this function is called.
 *
 * \param com       used to perform communication with device
 * \param data      pointer to buffer where read data is stored.
 * \param length    amount of data to read from mailbox
 * \return          returns true for success, false for failure
 */
bool WG0X::readMailboxInternal(EthercatCom *com, void *data, unsigned length)
{
  static const unsigned MAX_TRIES = 10;
  static const unsigned MAX_DROPPED = 10;
    
  if (length > MBX_STATUS_SIZE) {
    assert(length <= MBX_STATUS_SIZE);
    return false;
  }

  // Make sure slave is in correct state to use mailbox
  if (!verifyDeviceStateForMailboxOperation()){
    return false;
  }
    
  EC_Logic *logic = EC_Logic::instance();    
  EC_UINT station_addr = sh_->get_station_address();


  // If read is small enough :
  //  1. read just length bytes in one telegram
  //  2. then read last byte to empty mailbox
  static const unsigned TELEGRAM_OVERHEAD = 50;
  bool split_read = (length+TELEGRAM_OVERHEAD) < MBX_STATUS_SIZE;
    
  unsigned read_length = MBX_STATUS_SIZE;      
  if (split_read) {
    read_length = length;
 }

  unsigned char unused[1] = {0};
  NPRD_Telegram read_start(
                           logic->get_idx(),
                           station_addr,
                           MBX_STATUS_PHY_ADDR,
                           logic->get_wkc(),
                           read_length,
                           (unsigned char*) data);
  NPRD_Telegram read_end(  
                         logic->get_idx(),
                         station_addr,
                         MBX_STATUS_PHY_ADDR+MBX_STATUS_SIZE-1,
                         logic->get_wkc(),
                         sizeof(unused),
                         unused);      

  if (split_read) {
    read_start.attach(&read_end);
  }
    
  EC_Ethernet_Frame frame(&read_start);

  unsigned tries = 0;    
  unsigned total_dropped =0;
  for (tries=0; tries<MAX_TRIES; ++tries) {      

    // Send read - keep track of how many packets were dropped (for later)
    unsigned dropped=0;
    for (dropped=0; dropped<MAX_DROPPED; ++dropped) {
      if (com->txandrx_once(&frame)) {
        break;
      }
      ++total_dropped;
      updateIndexAndWkc(&read_start   , logic);
      updateIndexAndWkc(&read_end     , logic);
    }
      
    if (dropped>=MAX_DROPPED) {
      fprintf(stderr, "%s : " ERROR_HDR 
              " too many dropped packets : %d\n", __func__, dropped);
    }
      
    if (split_read && (read_start.get_wkc() != read_end.get_wkc())) {
      fprintf(stderr, "%s : " ERROR_HDR 
              "read mbx working counters are inconsistant\n", __func__);
      return false;
    }
      
    if (read_start.get_wkc() == 0) {
      if (dropped == 0) {
        fprintf(stderr, "%s : " ERROR_HDR 
                " inconsistancy : got wkc=%d with no dropped packets\n", 
                __func__, read_start.get_wkc()); 
        fprintf(stderr, "total dropped = %d\n", total_dropped);
        return false;
      } else {
        // Packet was dropped after doing read from device,,,
        // Ask device to repost data, so it can be read again.
        fprintf(stderr, "%s : " WARN_HDR 
                " asking for read repeat after dropping %d packets\n", __func__, dropped);
        if (!readMailboxRepeatRequest(com)) {
          return false;
        }
        continue;
      }
    } else if (read_start.get_wkc() == 1) {
      // Successfull read of status data
      break;
    } else {
      fprintf(stderr, "%s : " ERROR_HDR 
              " invalid wkc for read : %d\n", __func__, read_start.get_wkc());   
      diagnoseMailboxError(com);
      return false;
    }
  }

  if (tries >= MAX_TRIES) {
    fprintf(stderr, "%s : " ERROR_HDR 
            " could not get responce from device after %d retries, %d total dropped packets\n",
            __func__, tries, total_dropped);
    diagnoseMailboxError(com);
    return false;
  }        

  return true;
}


/*!
 * \brief  Read data from WG0X local bus using mailbox communication.
 *
 * Internally a localbus read is done in two parts.
 * First, a mailbox write of a command header that include local bus address and length.
 * Second, a mailbox read of the result.
 *
 * \param com       used to perform communication with device
 * \param address   WG0X (FPGA) local bus address to read from
 * \param data      pointer to buffer where read data can be stored, must be at least length in size
 * \param length    amount of data to read, limited at 511 bytes.
 * \return          returns zero for success, non-zero for failure 
 */
int WG0X::readMailbox(EthercatCom *com, unsigned address, void *data, unsigned length)
{
  if (!lockMailbox())
    return -1;

  int result = readMailbox_(com, address, data, length);
  if (result != 0) {
    ++mailbox_diagnostics_.read_errors_;
  }
  
  unlockMailbox();
  return result;
}

/*!
 * \brief  Internal function.  
 *
 * Aguments are the same as readMailbox() except that this assumes the mailbox lock is held.
 */ 
int WG0X::readMailbox_(EthercatCom *com, unsigned address, void *data, unsigned length)
{
  // Make sure slave is in correct state to use mailbox
  if (!verifyDeviceStateForMailboxOperation()){
    return false;
  }

  //  1. Clear read (status) mailbox by reading it first
  if (!clearReadMailbox(com)) 
  {
    fprintf(stderr, "%s : " ERROR_HDR 
            " clearing read mbx\n", __func__);
    return -1;
  }

  //  2. Put a (read) request into command mailbox
  {
    WG0XMbxCmd cmd;      
    if (!cmd.build(address, length, LOCAL_BUS_READ, sh_->get_mbx_counter(), data)) 
    {
      fprintf(stderr, "%s : " ERROR_HDR 
              " builing mbx header\n", __func__);
      return -1;
    }
    
    if (!writeMailboxInternal(com, &cmd.hdr_, sizeof(cmd.hdr_))) 
    {
      fprintf(stderr, "%s : " ERROR_HDR " write of cmd failed\n", __func__);
      return -1;
    }
  }
  
  // Wait for result (in read mailbox) to become ready
  if (!waitForReadMailboxReady(com)) 
  {
    fprintf(stderr, "%s : " ERROR_HDR 
            "waiting for read mailbox\n", __func__);
    return -1;
  }

  // Read result back from mailbox.
  // It could take the FPGA some time to respond to a request.  
  // Since the read mailbox is initiall cleared, any read to the mailbox
  // should be refused (WKC==0) until WG0x FPGA has written it result into it.	   
  // NOTE: For this to work the mailbox syncmanagers must be set up.
  // TODO 1: Packets may get lost on return route to device.
  //   In this case, the device will keep responding to the repeated packets with WKC=0.
  //   To work correctly, the repeat request bit needs to be toggled.
  // TODO 2: Need a better method to determine if data read from status mailbox.
  //   is the right data, or just junk left over from last time.
  { 
    WG0XMbxCmd stat;
    memset(&stat,0,sizeof(stat));
    // Read data + 1byte checksum from mailbox
    if (!readMailboxInternal(com, &stat, length+1)) 
    {
      fprintf(stderr, "%s : " ERROR_HDR " read failed\n", __func__);
      return -1;
    }
    
    if (computeChecksum(&stat, length+1) != 0) 
    {
      fprintf(stderr, "%s : " ERROR_HDR 
              "checksum error reading mailbox data\n", __func__);
      fprintf(stderr, "length = %d\n", length);
      return -1;
    }
    memcpy(data, &stat, length);
  }

  return 0;


}

bool WG0X::lockMailbox() 
{
  int error = pthread_mutex_lock(&mailbox_lock_);
  if (error != 0) {
    fprintf(stderr, "%s : " ERROR_HDR " getting mbx lock\n", __func__);
    ++mailbox_diagnostics_.lock_errors_;
    return false;
  }
  return true;
}

void WG0X::unlockMailbox() 
{
  int error = pthread_mutex_unlock(&mailbox_lock_);
  if (error != 0) {
    fprintf(stderr, "%s : " ERROR_HDR " freeing mbx lock\n", __func__);
    ++mailbox_diagnostics_.lock_errors_;
  }
}

bool WG0X::lockWG0XDiagnostics() 
{
  int error = pthread_mutex_lock(&wg0x_diagnostics_lock_);
  if (error != 0) {
    fprintf(stderr, "%s : " ERROR_HDR " getting diagnostics lock\n", __func__);
    // update error counters even if we didn't get lock
    ++wg0x_collect_diagnostics_.lock_errors_;
    return false;
  }
  return true;
}

bool WG0X::tryLockWG0XDiagnostics() 
{
  int error = pthread_mutex_trylock(&wg0x_diagnostics_lock_);
  if (error == EBUSY) {
    return false;
  }
  else if (error != 0) {
    fprintf(stderr, "%s : " ERROR_HDR " getting diagnostics lock\n", __func__);
    // update error counters even if we didn't get lock
    ++wg0x_collect_diagnostics_.lock_errors_;
    return false;
  }
  return true;
}

void WG0X::unlockWG0XDiagnostics() 
{
  int error = pthread_mutex_unlock(&wg0x_diagnostics_lock_);
  if (error != 0) {
    fprintf(stderr, "%s : " ERROR_HDR " freeing diagnostics lock\n", __func__);
    ++wg0x_collect_diagnostics_.lock_errors_;
  }
}


/*!
 * \brief  Write data to WG0X local bus using mailbox communication.
 *
 * First, this puts a command header that include local bus address and length in write mailbox.
 * Second it waits until device actually empties write mailbox.
 *
 * \param com       used to perform communication with device
 * \param address   WG0X (FPGA) local bus address to write data to
 * \param data      pointer to buffer where write data is stored, must be at least length in size
 * \param length    amount of data to write, limited at 507 bytes
 * \return          returns zero for success, non-zero for failure 
 */
int WG0X::writeMailbox(EthercatCom *com, unsigned address, void const *data, unsigned length)
{
  if (!lockMailbox())
    return -1;

  int result = writeMailbox_(com, address, data, length);
  if (result != 0) {
    ++mailbox_diagnostics_.write_errors_;
  }

  unlockMailbox();

  return result;
}

/*!
 * \brief  Internal function.  
 *
 * Aguments are the same as writeMailbox() except that this assumes the mailbox lock is held.
 */
int WG0X::writeMailbox_(EthercatCom *com, unsigned address, void const *data, unsigned length)
{
  // Make sure slave is in correct state to use mailbox
  if (!verifyDeviceStateForMailboxOperation()){
    return -1;
  }
    
  // Build message and put it into write mailbox
  {		
    WG0XMbxCmd cmd;
    if (!cmd.build(address, length, LOCAL_BUS_WRITE, sh_->get_mbx_counter(), data)) {
      fprintf(stderr, "%s : " ERROR_HDR " builing mbx header\n", __func__);
      return -1;
    }      
    
    unsigned write_length = sizeof(cmd.hdr_)+length+sizeof(cmd.checksum_);
    if (!writeMailboxInternal(com, &cmd, write_length)) {
      fprintf(stderr, "%s : " ERROR_HDR " write failed\n", __func__);
      diagnoseMailboxError(com);
      return -1;
    }
  }
  
  // TODO: Change slave firmware so that we can verify that localbus write was truly executed
  //  Checking that device emptied write mailbox will have to suffice for now.
  if (!waitForWriteMailboxReady(com)) {
    fprintf(stderr, "%s : " ERROR_HDR 
            "write mailbox\n", __func__);
  }
    
  return 0;
}


#define CHECK_SAFETY_BIT(bit) \
  do { if (status & SAFETY_##bit) { \
    str += prefix + #bit; \
    prefix = ", "; \
  } } while(0)

string WG0X::safetyDisableString(uint8_t status)
{
  string str, prefix;

  if (status & SAFETY_DISABLED)
  {
    CHECK_SAFETY_BIT(DISABLED);
    CHECK_SAFETY_BIT(UNDERVOLTAGE);
    CHECK_SAFETY_BIT(OVER_CURRENT);
    CHECK_SAFETY_BIT(BOARD_OVER_TEMP);
    CHECK_SAFETY_BIT(HBRIDGE_OVER_TEMP);
    CHECK_SAFETY_BIT(OPERATIONAL);
    CHECK_SAFETY_BIT(WATCHDOG);
  }
  else
    str = "ENABLED";

  return str;
}

string WG0X::modeString(uint8_t mode)
{
  string str, prefix;
  if (mode) {
    if (mode & MODE_ENABLE) {
      str += prefix + "ENABLE";
      prefix = ", ";
    }
    if (mode & MODE_CURRENT) {
      str += prefix + "CURRENT";
      prefix = ", ";
    }
    if (mode & MODE_UNDERVOLTAGE) {
      str += prefix + "UNDERVOLTAGE";
      prefix = ", ";
    }
    if (mode & MODE_SAFETY_RESET) {
      str += prefix + "SAFETY_RESET";
      prefix = ", ";
    }
    if (mode & MODE_SAFETY_LOCKOUT) {
      str += prefix + "SAFETY_LOCKOUT";
      prefix = ", ";
    }
    if (mode & MODE_RESET) {
      str += prefix + "RESET";
      prefix = ", ";
    }
  } else {
    str = "OFF";
  }
  return str;
}

void WG0X::publishMailboxDiagnostics(diagnostic_updater::DiagnosticStatusWrapper &d)
{
  if (lockMailbox()) { 
    mailbox_publish_diagnostics_ = mailbox_diagnostics_;
    unlockMailbox();
  }

  MbxDiagnostics const &m(mailbox_publish_diagnostics_);
  d.addf("Mailbox Write Errors", "%d", m.write_errors_);
  d.addf("Mailbox Read Errors", "%d",  m.read_errors_);
  d.addf("Mailbox Retries", "%d",      m.retries_);
  d.addf("Mailbox Retry Errors", "%d", m.retry_errors_);
}

void WG0X::publishGeneralDiagnostics(diagnostic_updater::DiagnosticStatusWrapper &d)
{ 
  // If possible, copy new diagnositics from collection thread, into diagnostics thread
  if (tryLockWG0XDiagnostics()) { 
    wg0x_publish_diagnostics_ = wg0x_collect_diagnostics_;
    unlockWG0XDiagnostics(); 
  }

  if (too_many_dropped_packets_)
    d.mergeSummary(d.ERROR, "Too many dropped packets");

  if (status_checksum_error_)
  {
    d.mergeSummary(d.ERROR, "Checksum error on status data");
  }
  
  if (wg0x_publish_diagnostics_.first_)
  {
    d.mergeSummary(d.WARN, "Have not yet collected WG0X diagnostics");
  }
  else if (!wg0x_publish_diagnostics_.valid_) 
  {
    d.mergeSummary(d.WARN, "Could not collect WG0X diagnostics");
  }

  WG0XDiagnostics const &p(wg0x_publish_diagnostics_);
  WG0XSafetyDisableStatus const &s(p.safety_disable_status_);
  d.addf("Status Checksum Error Count", "%d", p.checksum_errors_);
  d.addf("Safety Disable Status", "%s (%02x)", safetyDisableString(s.safety_disable_status_).c_str(), s.safety_disable_status_);
  d.addf("Safety Disable Status Hold", "%s (%02x)", safetyDisableString(s.safety_disable_status_hold_).c_str(), s.safety_disable_status_hold_);
  d.addf("Safety Disable Count", "%d", p.safety_disable_total_);
  d.addf("Undervoltage Count", "%d", p.undervoltage_total_);
  d.addf("Over Current Count", "%d", p.over_current_total_);
  d.addf("Board Over Temp Count", "%d", p.board_over_temp_total_);
  d.addf("Bridge Over Temp Count", "%d", p.bridge_over_temp_total_);
  d.addf("Operate Disable Count", "%d", p.operate_disable_total_);
  d.addf("Watchdog Disable Count", "%d", p.watchdog_disable_total_);

  if (in_lockout_)
  {
    uint8_t status = s.safety_disable_status_hold_;
    string prefix(": "); 
    string str("Safety Lockout");
    CHECK_SAFETY_BIT(UNDERVOLTAGE);
    CHECK_SAFETY_BIT(OVER_CURRENT);
    CHECK_SAFETY_BIT(BOARD_OVER_TEMP);
    CHECK_SAFETY_BIT(HBRIDGE_OVER_TEMP);
    CHECK_SAFETY_BIT(OPERATIONAL);
    CHECK_SAFETY_BIT(WATCHDOG);
    d.mergeSummary(d.ERROR, str);
  }

  if (timestamp_jump_detected_ && (s.safety_disable_status_hold_ & SAFETY_OPERATIONAL))
  {
    fpga_internal_reset_detected_ = true;
  }

  if (fpga_internal_reset_detected_) 
  {
    d.mergeSummaryf(d.ERROR, "FPGA internal reset detected");
  }
  
  if (timestamp_jump_detected_)
  {
    d.mergeSummaryf(d.WARN, "Timestamp jumped");
  }

  {

    const WG0XDiagnosticsInfo &di(p.diagnostics_info_);
    //d.addf("PDO Command IRQ Count", "%d", di.pdo_command_irq_count_);
    d.addf("MBX Command IRQ Count", "%d", di.mbx_command_irq_count_);
    d.addf("PDI Timeout Error Count", "%d", di.pdi_timeout_error_count_);
    d.addf("PDI Checksum Error Count", "%d", di.pdi_checksum_error_count_);
    unsigned product = sh_->get_product_code();

    // Current scale 
    if ((product == WG05_PRODUCT_CODE) && (board_major_ == 1))
    {
      // WG005B measure current going into and out-of H-bridge (not entire board)
      static const double WG005B_SUPPLY_CURRENT_SCALE = (1.0 / (8152.0 * 0.851)) * 4.0;
      double bridge_supply_current = double(di.supply_current_in_) * WG005B_SUPPLY_CURRENT_SCALE;
      d.addf("Bridge Supply Current", "%f", bridge_supply_current);
    }
    else if ((product == WG05_PRODUCT_CODE) || (product == WG021_PRODUCT_CODE)) 
    {
      // WG005[CDEF] measures curret going into entire board.  It cannot measure negative (regenerative) current values.
      // WG021A == WG005E,  WG021B == WG005F
      static const double WG005_SUPPLY_CURRENT_SCALE = ((82.0 * 2.5) / (0.01 * 5100.0 * 32768.0));
      double supply_current = double(di.supply_current_in_) * WG005_SUPPLY_CURRENT_SCALE;
      d.addf("Supply Current", "%f",  supply_current);
    }
    d.addf("Configured Offset A", "%f", config_info_.nominal_current_scale_ * di.config_offset_current_A_);
    d.addf("Configured Offset B", "%f", config_info_.nominal_current_scale_ * di.config_offset_current_B_);
  }
}



void WG0X::diagnostics(diagnostic_updater::DiagnosticStatusWrapper &d, unsigned char *buffer)
{
  WG0XStatus *status = (WG0XStatus *)(buffer + command_size_);

  stringstream str;
  str << "EtherCAT Device (" << actuator_info_.name_ << ")";
  d.name = str.str();
  char serial[32];
  snprintf(serial, sizeof(serial), "%d-%05d-%05d", config_info_.product_id_ / 100000 , config_info_.product_id_ % 100000, config_info_.device_serial_number_);
  d.hardware_id = serial;

  if (!has_error_)
    d.summary(d.OK, "OK");

  d.clear();
  d.add("Configuration", config_info_.configuration_status_ ? "good" : "error loading configuration");
  d.add("Name", actuator_info_.name_);
  d.addf("Position", "%02d", sh_->get_ring_position());
  d.addf("Product code",
        "WG0%d (%d) Firmware Revision %d.%02d, PCB Revision %c.%02d",
        sh_->get_product_code() == WG05_PRODUCT_CODE ? 5 : 6,
        sh_->get_product_code(), fw_major_, fw_minor_,
        'A' + board_major_, board_minor_);

  d.add("Robot", actuator_info_.robot_name_);
  d.addf("Motor", "%s %s", actuator_info_.motor_make_, actuator_info_.motor_model_);
  d.add("Serial Number", serial);
  d.addf("Nominal Current Scale", "%f",  config_info_.nominal_current_scale_);
  d.addf("Nominal Voltage Scale",  "%f", config_info_.nominal_voltage_scale_);
  d.addf("HW Max Current", "%f", config_info_.absolute_current_limit_ * config_info_.nominal_current_scale_);

  d.addf("SW Max Current", "%f", actuator_info_.max_current_);
  d.addf("Speed Constant", "%f", actuator_info_.speed_constant_);
  d.addf("Resistance", "%f", actuator_info_.resistance_);
  d.addf("Motor Torque Constant", "%f", actuator_info_.motor_torque_constant_);
  d.addf("Pulses Per Revolution", "%d", actuator_info_.pulses_per_revolution_);
  d.addf("Encoder Reduction", "%f", actuator_info_.encoder_reduction_);

  publishGeneralDiagnostics(d);
  publishMailboxDiagnostics(d);

  d.addf("Calibration Offset", "%f", cached_zero_offset_);
  d.addf("Calibration Status", "%s", 
         (calibration_status_ == NO_CALIBRATION) ? "No calibration" :
         (calibration_status_ == CONTROLLER_CALIBRATION) ? "Calibrated by controller" :
         (calibration_status_ == SAVED_CALIBRATION) ? "Using saved calibration" : "UNKNOWN");

  d.addf("Watchdog Limit", "%dms", config_info_.watchdog_limit_);
  d.add("Mode", modeString(status->mode_));
  d.addf("Digital out", "%d", status->digital_out_);
  d.addf("Programmed pwm value", "%d", status->programmed_pwm_value_);
  d.addf("Programmed current", "%f", status->programmed_current_ * config_info_.nominal_current_scale_);
  d.addf("Measured current", "%f", status->measured_current_ * config_info_.nominal_current_scale_);
  d.addf("Timestamp", "%u", status->timestamp_);
  d.addf("Encoder count", "%d", status->encoder_count_);
  d.addf("Encoder index pos", "%d", status->encoder_index_pos_);
  d.addf("Num encoder_errors", "%d", status->num_encoder_errors_);
  d.addf("Encoder status", "%d", status->encoder_status_);
  d.addf("Calibration reading", "%d", status->calibration_reading_);
  d.addf("Last calibration rising edge", "%d", status->last_calibration_rising_edge_);
  d.addf("Last calibration falling edge", "%d", status->last_calibration_falling_edge_);
  d.addf("Board temperature", "%f", 0.0078125 * status->board_temperature_);
  d.addf("Max board temperature", "%f", 0.0078125 * max_board_temperature_);
  d.addf("Bridge temperature", "%f", 0.0078125 * status->bridge_temperature_);
  d.addf("Max bridge temperature", "%f", 0.0078125 * max_bridge_temperature_);
  d.addf("Supply voltage", "%f", status->supply_voltage_ * config_info_.nominal_voltage_scale_);
  d.addf("Motor voltage", "%f", status->motor_voltage_ * config_info_.nominal_voltage_scale_);
  d.addf("Current Loop Kp", "%d", config_info_.current_loop_kp_);
  d.addf("Current Loop Ki", "%d", config_info_.current_loop_ki_);

  if (motor_model_) 
  {
    motor_model_->diagnostics(d);
    if (disable_motor_model_checking_)
    {
      d.mergeSummaryf(d.WARN, "Motor model disabled");      
    }
  }

  if (motor_heating_model_.get() != NULL)
  {
    motor_heating_model_->diagnostics(d);
  }

  if (last_num_encoder_errors_ != status->num_encoder_errors_)
  {
    d.mergeSummaryf(d.WARN, "Encoder errors detected");
  }

  d.addf("Packet count", "%d", status->packet_count_);

  d.addf("Drops", "%d", drops_);
  d.addf("Consecutive Drops", "%d", consecutive_drops_);
  d.addf("Max Consecutive Drops", "%d", max_consecutive_drops_);

  unsigned numPorts = (sh_->get_product_code()==WG06_PRODUCT_CODE) ? 1 : 2; // WG006 has 1 port, WG005 has 2
  EthercatDevice::ethercatDiagnostics(d, numPorts); 
}

