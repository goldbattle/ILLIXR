#include <vector>
#include <chrono>
#include <math.h>
#include "kalman.hh"
#include "common/component.hh"
#include "common/switchboard.hh"
#include "common/data_format.hh"

using namespace ILLIXR;

class pose_prediction : public component {
public:
    // Public constructor, create_component passes Switchboard handles ("plugs")
    // to this constructor. In turn, the constructor fills in the private
    // references to the switchboard plugs, so the component can read the
    // data whenever it needs to.
    pose_prediction(std::unique_ptr<reader_latest<pose_type>>&& pose_plug,
      std::unique_ptr<reader_latest<bool>>&& slam_ready,
	  std::unique_ptr<reader_latest<imu_type>>&& imu_plug,
      std::unique_ptr<writer<pose_type>>&& predicted_pose_plug)
	: _m_slow_pose{std::move(pose_plug)}
    , _m_slam_ready{std::move(slam_ready)}
	, _m_imu{std::move(imu_plug)}
    , _m_fast_pose{std::move(predicted_pose_plug)} {

        std::chrono::time_point<std::chrono::system_clock> current_time = std::chrono::system_clock::now();
        _latest_pose = pose_type{current_time, Eigen::Vector3f{0, 0, 6}, Eigen::Quaternionf{1, 0, 0, 0}};
        _last_slow_pose_update_time = current_time;

        _filter = new kalman_filter{current_time};
        _current_slam_position = Eigen::Vector3f{0, 0, 6};
        _latest_vel = Eigen::Vector3f{0, 0, 0};
        _latest_acc = Eigen::Vector3f{0, 0, 0};
        _latest_gyro = Eigen::Vector3f{0, 0, 0};
    }

    // Overridden method from the component interface. This specifies one interation of the main loop 
    virtual void _p_compute_one_iteration() override {
        auto is_slam_ready = _m_slam_ready->get_latest_ro();
        assert(is_slam_ready != NULL);
        if (*is_slam_ready == false) {
            return;
        }

		auto pose_sample_nullable = _m_slow_pose->get_latest_ro();
		assert(pose_sample_nullable != NULL);

        // If the SB has a new slow pose value from SLAM
        const pose_type fresh_pose = *pose_sample_nullable;
        float time_diff = static_cast<float>(std::chrono::duration_cast<std::chrono::nanoseconds>
			    						   (fresh_pose.time - _latest_pose.time).count()) / 1000000000.0f;
        if (fresh_pose.time > _last_slow_pose_update_time) {
            // std::cout << "Time between new and old measurement: " << time_diff << std::endl;
            _update_slow_pose(fresh_pose);
        }

        const imu_type* fresh_imu_measurement = _m_imu->get_latest_ro();
		assert(fresh_imu_measurement != NULL);

        // If queried IMU is fresher than the pose mesaurement and the local IMU copy we have, update it and the pose
        if (fresh_imu_measurement->time > _latest_pose.time) {
            _update_fast_pose(*fresh_imu_measurement);
        }
    }

    /*
    This developer is responsible for killing their processes
    and deallocating their resources here.
    */
    virtual ~pose_prediction() override {}

private:
    // Switchboard plugs for writing / reading data.
    std::unique_ptr<reader_latest<pose_type>> _m_slow_pose;
    std::unique_ptr<reader_latest<bool>> _m_slam_ready;
    std::unique_ptr<reader_latest<imu_type>> _m_imu;
    std::unique_ptr<writer<pose_type>> _m_fast_pose;

    pose_type _latest_pose;
    std::chrono::time_point<std::chrono::system_clock> _last_slow_pose_update_time;

    kalman_filter* _filter;
    Eigen::Vector3f _current_slam_position;
    Eigen::Vector3f _latest_vel;
    Eigen::Vector3f _latest_acc;
    Eigen::Vector3f _latest_gyro;

    // Helper that will push to the SB if a newer pose is pulled from the SB/SLAM
    void _update_slow_pose(const pose_type fresh_pose) {
        float time_difference = static_cast<float>(std::chrono::duration_cast<std::chrono::nanoseconds>
												   (fresh_pose.time - _last_slow_pose_update_time).count()) / 1000000000.0f;

        // Update the velocity with the pose position difference over time. This is done because
        // Accelerometer readings have lots of noise and will lead to bad dead reckoning
        _latest_vel[0] = (fresh_pose.position[0] - _current_slam_position[0]) / time_difference;
        _latest_vel[1] = (fresh_pose.position[1] - _current_slam_position[1]) / time_difference;
        _latest_vel[2] = (fresh_pose.position[2] - _current_slam_position[2]) / time_difference;

        _current_slam_position = fresh_pose.position;
        _latest_pose = fresh_pose;
        _last_slow_pose_update_time = fresh_pose.time;

        std::cout << "New pose recieved from SLAM!" << std::endl;
        _m_fast_pose->put(new pose_type(_latest_pose));
    }

    // Helper that updates the pose with new IMU readings and pushed to the SB
    void _update_fast_pose(const imu_type fresh_imu_measurement) {
        _latest_acc = Eigen::Vector3f{
            fresh_imu_measurement.linear_a[0], 
            fresh_imu_measurement.linear_a[1], 
            fresh_imu_measurement.linear_a[2]
        };

        // _latest_gyro = _filter->predict_values(fresh_imu_measurement);
        _latest_gyro = Eigen::Vector3f(fresh_imu_measurement.angular_v[0], fresh_imu_measurement.angular_v[1], fresh_imu_measurement.angular_v[2]);

        float time_difference = static_cast<float>(std::chrono::duration_cast<std::chrono::nanoseconds>
                                (fresh_imu_measurement.time - _latest_pose.time).count()) / 1000000000.0f;

        _latest_vel[0] += _latest_acc[0] * time_difference;
        _latest_vel[1] += _latest_acc[1] * time_difference;
        _latest_vel[2] += _latest_acc[2] * time_difference;

        _latest_pose = _calculate_pose(time_difference);
        _latest_pose.time = fresh_imu_measurement.time;

		// std::cout << "latest linear acc " << _latest_acc[0] << ", " << _latest_acc[1] << ", " << _latest_acc[2] << std::endl;
        // std::cout << "latest vel " << _latest_vel[0] << ", " << _latest_vel[1] << ", " << _latest_vel[2] << std::endl;
        std::cout << "Predicted Orientation " << _latest_gyro[0] << ", " << _latest_gyro[1] << ", " << _latest_gyro[2] << std::endl;
		std::cout << "Predicted Position " << _latest_pose.position[0] << ", " << _latest_pose.position[1] << ", " << _latest_pose.position[2] << std::endl;
		
        assert(isfinite(_latest_pose.orientation.w()));
        assert(isfinite(_latest_pose.orientation.x()));
        assert(isfinite(_latest_pose.orientation.y()));
        assert(isfinite(_latest_pose.orientation.z()));
        assert(isfinite(_latest_pose.position[0]));
        assert(isfinite(_latest_pose.position[1]));
        assert(isfinite(_latest_pose.position[2]));

        _m_fast_pose->put(new pose_type(_latest_pose));
    }

    // Helper that does all the math to calculate the poses
    pose_type _calculate_pose(float time_delta) {

        // Convert the quaternion to euler angles and calculate the new rotation
        Eigen::Vector3f orientation_euler = _latest_pose.orientation.toRotationMatrix().eulerAngles(0, 1, 2);
        orientation_euler(0) += _latest_gyro[0] * time_delta;
        orientation_euler(1) += _latest_gyro[1] * time_delta;
        orientation_euler(2) += _latest_gyro[2] * time_delta;

        // Convert euler angles back into a quaternion
        Eigen::Quaternionf predicted_orientation = Eigen::AngleAxisf(orientation_euler(0), Eigen::Vector3f::UnitX())
                * Eigen::AngleAxisf(orientation_euler(1), Eigen::Vector3f::UnitY())
                * Eigen::AngleAxisf(orientation_euler(2), Eigen::Vector3f::UnitZ());

        // Calculate the new pose transform by .5*a*t^2 + v*t + d
        Eigen::Vector3f predicted_position = {
            static_cast<float>(0.5 * _latest_acc[0] * pow(time_delta, 2) + _latest_vel[0] * time_delta + _latest_pose.position[0]),
            static_cast<float>(0.5 * _latest_acc[1] * pow(time_delta, 2) + _latest_vel[1] * time_delta + _latest_pose.position[1]),
            static_cast<float>(0.5 * _latest_acc[2] * pow(time_delta, 2) + _latest_vel[2] * time_delta + _latest_pose.position[2]),
        };

        return pose_type{_latest_pose.time, predicted_position, predicted_orientation};
    }
};

extern "C" component* create_component(switchboard* sb) {
	/* First, we declare intent to read/write topics. Switchboard
	   returns handles to those topics. */
    auto slow_pose_plug = sb->subscribe_latest<pose_type>("slow_pose");
    auto slam_ready = sb->subscribe_latest<bool>("slam_ready");
	auto imu_plug = sb->subscribe_latest<imu_type>("imu0");
	auto fast_pose_plug = sb->publish<pose_type>("fast_pose");

	/* This ensures pose is available at startup. */
	auto nullable_pose = slow_pose_plug->get_latest_ro();
	assert(nullable_pose != NULL);
	fast_pose_plug->put(nullable_pose);

	return new pose_prediction{std::move(slow_pose_plug), std::move(slam_ready), std::move(imu_plug), std::move(fast_pose_plug)};
}
