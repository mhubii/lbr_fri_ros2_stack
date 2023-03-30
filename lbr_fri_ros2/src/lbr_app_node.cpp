#include "lbr_fri_ros2/lbr_app_node.hpp"

namespace lbr_fri_ros2 {
LBRAppNode::LBRAppNode(const std::string &node_name, const int &port_id,
                       const char *const remote_host)
    : rclcpp::Node(node_name) {
  if (!valid_port_(port_id)) {
    throw std::range_error("Invalid port_id provided.");
  }
  port_id_ = port_id;
  remote_host_ = remote_host;

  connected_ = false;

  app_connect_srv_ = create_service<lbr_fri_msgs::srv::AppConnect>(
      "~/connect",
      std::bind(&LBRAppNode::app_connect_cb_, this, std::placeholders::_1, std::placeholders::_2),
      rmw_qos_profile_services_default);

  app_disconnect_srv_ = create_service<lbr_fri_msgs::srv::AppDisconnect>(
      "~/disconnect",
      std::bind(&LBRAppNode::app_disconnect_cb_, this, std::placeholders::_1,
                std::placeholders::_2),
      rmw_qos_profile_services_default);

  lbr_command_rt_buf_ =
      std::make_shared<realtime_tools::RealtimeBuffer<lbr_fri_msgs::msg::LBRCommand::SharedPtr>>(
          nullptr);
  lbr_command_sub_ = create_subscription<lbr_fri_msgs::msg::LBRCommand>(
      "/lbr_command", rclcpp::SensorDataQoS(),
      std::bind(&LBRAppNode::lbr_command_sub_cb_, this, std::placeholders::_1));
  lbr_state_pub_ =
      create_publisher<lbr_fri_msgs::msg::LBRState>("/lbr_state", rclcpp::SensorDataQoS());
  lbr_state_rt_pub_ =
      std::make_shared<realtime_tools::RealtimePublisher<lbr_fri_msgs::msg::LBRState>>(
          lbr_state_pub_);

  std::string robot_description;
  declare_parameter<std::string>("robot_description");
  if (!get_parameter("robot_description", robot_description)) {
    throw std::runtime_error("Failed to receive robot_description parameter.");
  }

  lbr_intermediary_ = std::make_shared<LBRIntermediary>(LBRCommandGuard{robot_description});
  lbr_client_ = std::make_shared<LBRClient>(lbr_intermediary_);
  connection_ = std::make_unique<KUKA::FRI::UdpConnection>();
  app_ = std::make_unique<KUKA::FRI::ClientApplication>(*connection_, *lbr_client_);

  // attempt default connect
  connect_(port_id_, remote_host_);
}

LBRAppNode::~LBRAppNode() { disconnect_(); }

void LBRAppNode::app_connect_cb_(const lbr_fri_msgs::srv::AppConnect::Request::SharedPtr request,
                                 lbr_fri_msgs::srv::AppConnect::Response::SharedPtr response) {
  const char *remote_host = request->remote_host.empty() ? NULL : request->remote_host.c_str();
  try {
    response->connected = connect_(request->port_id, remote_host);
  } catch (const std::exception &e) {
    response->message = e.what();
    RCLCPP_ERROR(get_logger(), "Failed. %s", e.what());
  }
}

void LBRAppNode::app_disconnect_cb_(
    const lbr_fri_msgs::srv::AppDisconnect::Request::SharedPtr /*request*/,
    lbr_fri_msgs::srv::AppDisconnect::Response::SharedPtr response) {
  try {
    response->disconnected = disconnect_();
  } catch (const std::exception &e) {
    response->message = e.what();
    RCLCPP_ERROR(get_logger(), "Failed. %s", e.what());
  }
}

void LBRAppNode::lbr_command_sub_cb_(const lbr_fri_msgs::msg::LBRCommand::SharedPtr lbr_command) {
  lbr_command_rt_buf_->writeFromNonRT(lbr_command);
}

bool LBRAppNode::valid_port_(const int &port_id) {
  if (port_id < 30200 || port_id > 30209) {
    RCLCPP_ERROR(get_logger(), "Expected port_id id in [30200, 30209], got %d.", port_id);
    return false;
  }
  return true;
}

bool LBRAppNode::connect_(const int &port_id, const char *const remote_host) {
  RCLCPP_INFO(get_logger(), "Attempting to open UDP socket for LBR server...");
  if (!connected_) {
    if (!valid_port_(port_id)) {
      throw std::range_error("Invalid port_id provided.");
    }
    connected_ = app_->connect(port_id, remote_host);
    if (connected_) {
      port_id_ = port_id;
      remote_host_ = remote_host;
      app_step_thread_ = std::make_unique<std::thread>(std::bind(&LBRAppNode::step_, this));
    }
  } else {
    RCLCPP_INFO(get_logger(), "Port already open.");
  }
  if (connected_) {
    RCLCPP_INFO(get_logger(), "Opened successfully.");
  } else {
    RCLCPP_WARN(get_logger(), "Failed to open.");
  }
  return connected_;
}

bool LBRAppNode::disconnect_() {
  RCLCPP_INFO(get_logger(), "Attempting to close UDP socket for LBR server...");
  if (connected_) {
    app_->disconnect();
    connected_ = false;
  } else {
    RCLCPP_INFO(get_logger(), "Port already closed.");
  }
  if (!connected_) {
    RCLCPP_INFO(get_logger(), "Closed successfully.");
  } else {
    RCLCPP_WARN(get_logger(), "Failed to close.");
  }
  auto future = std::async(std::launch::async, &std::thread::join, app_step_thread_.get());
  if (future.wait_for(std::chrono::seconds(1)) != std::future_status::ready) {
    throw std::runtime_error("Could not join app step thread.");
  }
  app_step_thread_.release();
  return !connected_;
}

void LBRAppNode::step_() {
  bool success = true;
  while (success && connected_ && rclcpp::ok()) {
    try {
      if (lbr_intermediary_->lbr_state().session_state ==
          KUKA::FRI::ESessionState::COMMANDING_WAIT) {
        lbr_command_rt_buf_->reset();
      }
      auto lbr_command = *lbr_command_rt_buf_->readFromRT();
      lbr_intermediary_->command_to_buffer(lbr_command);
      success = app_->step();
      if (lbr_state_rt_pub_->trylock()) {
        lbr_intermediary_->buffer_to_state(lbr_state_rt_pub_->msg_);
        lbr_state_rt_pub_->unlockAndPublish();
      }
    } catch (const std::exception &e) {
      RCLCPP_ERROR(get_logger(), e.what());
      break;
    }
  }
  if (connected_) {
    disconnect_();
  }
}
} // end of namespace lbr_fri_ros2
