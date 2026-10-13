// forwards every PointCloud2 on a topic to the server's socket:// source
//
// parameters
//   topic   PointCloud2 topic, default /points
//   server  host:port, default 127.0.0.1:9500

#include "socket_sender.h"

#include <chrono>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>

namespace
{
    class bridge_node_t : public rclcpp::Node
    {
      public:
        bridge_node_t() : rclcpp::Node("robv_bridge")
        {
            topic = declare_parameter<std::string>("topic", "/points");
            server = declare_parameter<std::string>("server", "127.0.0.1:9500");

            if (!robv::socket_sender::init(sender, server))
            {
                throw std::invalid_argument("server must be ipv4:port, got '" + server + "'");
            }

            // best effort, newest wins
            subscription = create_subscription<sensor_msgs::msg::PointCloud2>(
                topic, rclcpp::SensorDataQoS(),
                [this](sensor_msgs::msg::PointCloud2::ConstSharedPtr msg) { on_cloud(*msg); });

            // retries once a second so a missing server shows in the log early
            timer = create_wall_timer(std::chrono::seconds(1), [this]() { on_timer(); });

            RCLCPP_INFO(get_logger(), "forwarding %s to socket://%s", topic.c_str(), server.c_str());
        }

        ~bridge_node_t() override { robv::socket_sender::shutdown(sender); }

      private:
        // stub: converting to point_cloud_frame_t and calling socket_sender::send land with the real subscriber
        void on_cloud(const sensor_msgs::msg::PointCloud2& msg)
        {
            ++clouds_received;
            last_point_count = static_cast<u64>(msg.width) * msg.height;
        }

        void on_timer()
        {
            const bool was_connected = robv::socket_sender::is_connected(sender);
            const bool connected = robv::socket_sender::ensure_connected(sender);
            if (connected != was_connected)
            {
                RCLCPP_INFO(get_logger(), "%s robv-server at %s", connected ? "connected to" : "lost", server.c_str());
            }
            RCLCPP_INFO_THROTTLE(
                get_logger(), *get_clock(), 10000, "%llu clouds received (last %llu points), %llu sent, server %s",
                static_cast<unsigned long long>(clouds_received), static_cast<unsigned long long>(last_point_count),
                static_cast<unsigned long long>(sender.frames_sent), connected ? "connected" : "not reachable");
        }

        std::string topic;
        std::string server;
        robv::socket_sender_t sender;
        u64 clouds_received = 0;
        u64 last_point_count = 0;
        rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr subscription;
        rclcpp::TimerBase::SharedPtr timer;
    };
} // namespace

int main(int argc, char** argv)
{
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<bridge_node_t>());
    rclcpp::shutdown();
    return 0;
}
