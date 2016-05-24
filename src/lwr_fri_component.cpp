#include <gazebo/gazebo.hh>
#include <gazebo/physics/physics.hh>
#include <gazebo/common/common.hh>

#include <rtt/Component.hpp>
#include <rtt/Port.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/Logger.hpp>
#include <rtt/Property.hpp>
#include <rtt/Attribute.hpp>

//#include <Eigen/Dense>
#include <boost/graph/graph_concepts.hpp>
#include <rtt/os/Timer.hpp>
#include <rtt/os/TimeService.hpp>
#include <boost/thread/mutex.hpp>
#include <friremote_rt.h>
//#include <friudp_rt.h>

#include <rci/dto/JointAngles.h>
#include <rci/dto/JointTorques.h>
#include <rci/dto/JointVelocities.h>

#include <nemo/Vector.h>
#include <nemo/Mapping.h>

#define l(lvl) RTT::log(lvl) << "[" << this->getName() << "] "

class LWRFriComponent: public RTT::TaskContext {
public:

	LWRFriComponent(std::string const& name) :
			RTT::TaskContext(name), steps_rtt_(0), steps_gz_(0), rtt_done(
			true), gazebo_done(false), new_data(true), cnt_lock_(100), last_steps_rtt_(
					0), set_new_pos(false), nb_no_data_(0), set_brakes(false), nb_static_joints(
					0), // HACK: The urdf has static tag for base_link, which makes it appear in gazebo as a joint
			last_gz_update_time_(0), nb_cmd_received_(0), sync_with_cmds_(true) {
		// Add required gazebo interfaces
//		this->provides("gazebo")->addOperation("configure",
//				&LWRGazeboComponent::gazeboConfigureHook, this,
//				RTT::ClientThread);
//		this->provides("gazebo")->addOperation("update",
//				&LWRGazeboComponent::gazeboUpdateHook, this, RTT::ClientThread);

//		this->addOperation("setLinkGravityMode",&LWRGazeboComponent::setLinkGravityMode,this,RTT::ClientThread); // TODO

		this->ports()->addPort("JointPositionCommand",
				port_JointPositionCommand).doc(
				"Input for JointPosition-cmds from Orocos to Gazebo world.");
		this->ports()->addPort("JointTorqueCommand", port_JointTorqueCommand).doc(
				"Input for JointTorque-cmds from Orocos to Gazebo world.");
		this->ports()->addPort("JointVelocityCommand",
				port_JointVelocityCommand).doc(
				"Input for JointVelocity-cmds from Orocos to Gazebo world.");

		this->ports()->addPort("JointVelocity", port_JointVelocity).doc(
				"Output for JointVelocity-fbs from Gazebo to Orocos world.");
		this->ports()->addPort("JointTorque", port_JointTorque).doc(
				"Output for JointTorques-fbs from Gazebo to Orocos world.");
		this->ports()->addPort("JointPosition", port_JointPosition).doc(
				"Output for JointPosition-fbs from Gazebo to Orocos world.");

		this->provides("debug")->addAttribute("jnt_pos", jnt_pos_);
		this->provides("debug")->addAttribute("jnt_vel", jnt_vel_);
		this->provides("debug")->addAttribute("jnt_trq", jnt_trq_);
		this->provides("debug")->addAttribute("gz_time", gz_time_);
		this->provides("debug")->addAttribute("write_duration",
				write_duration_);
		this->provides("debug")->addAttribute("read_duration", read_duration_);
		this->provides("debug")->addAttribute("rtt_time", rtt_time_);
		this->provides("debug")->addAttribute("steps_rtt", steps_rtt_);
		this->provides("debug")->addAttribute("steps_gz", steps_gz_);
		this->provides("debug")->addAttribute("period_sim", period_sim_);
		this->provides("debug")->addAttribute("period_wall", period_wall_);

		this->provides("misc")->addAttribute("urdf_string", urdf_string);
		RTT::log().setStdStream(std::cout);
		RTT::log().allowRealTime();
		RTT::log().startup();
		RTT::log(RTT::Info)<<"ARGH!\n"<<RTT::endlog();
	}

	virtual void updateHook() {

		double rtt_time_now_ = 0;
		double rtt_last_clock = 0;

		do {
RTT::log(RTT::Error)<<"Updating data!\n"<<RTT::endlog();
			updateData();
			if ((nb_cmd_received_ == 0 && data_fs == RTT::NoData)
					|| jnt_pos_fs == RTT::NewData)
				break;

			rtt_time_now_ = 1E-9
					* RTT::os::TimeService::ticks2nsecs(
							RTT::os::TimeService::Instance()->getTicks());

			if (rtt_time_now_ != rtt_last_clock && data_fs != RTT::NewData)
				//				RTT::log(RTT::Debug) << getName() << " "
				//						<< "Waiting for UpdateHook at " << rtt_time_now_
				//						<< " v:" << nb_cmd_received_ << data_fs
				//						<< RTT::endlog();
				rtt_last_clock = rtt_time_now_;

			//			RTT::log(RTT::Debug) << getName() << " nb_cmd_received_ = "
			//					<< nb_cmd_received_ << RTT::endlog();

			//while no new data for jnt_trqs, and no other commands recieved
			//maybe actually turn off for asynchronise control?
		} while (!(RTT::NewData == data_fs && nb_cmd_received_)
				&& sync_with_cmds_);
		RTT::log(RTT::Error)<<"ARGH!!!\n"<<RTT::endlog();
		// Increment simulation step counter (debugging)
		//steps_gz_++;

		// Get the RTT and gazebo time for debugging purposes
		rtt_time_ = 1E-9
				* RTT::os::TimeService::ticks2nsecs(
						RTT::os::TimeService::Instance()->getTicks());
		//TODO get total time maybe?

		// Get state
		//Standard FRI!
		RTT::log(RTT::Error)<<"recieve\n"<<RTT::endlog();
		int res = -1, count = 0;
		do{
		res = friInst->doReceiveData();
		RTT::log(RTT::Error)<<"No connection to KRC unit retrying in 3 seconds!"<<RTT::endlog();
		count++;
		sleep(3);
		}while(res==-1 && count<3);
		if(res==-1){
			RTT::log(RTT::Error)<<"No connection to KRC unit! Stopping Component."<<RTT::endlog();
			this->stop();
		}
		friInst->setToKRLInt(0,1);
		lastQuality = friInst->getQuality();
		if(lastQuality >= FRI_QUALITY_OK){
			friInst->setToKRLInt(0,10);
		}
		friInst->setToKRLReal(0,friInst->getFrmKRLReal(1));
		//END STANDARD
		//TODO check for fri quality!!
		float* pos = friInst->getMsrCmdJntPosition();
		for (unsigned j = 0; j < joints_idx.size(); j++) {

//			jnt_pos_->setFromRad(j,
//					gazebo_joints_[joints_idx[j]]->GetAngle(0).Radian());
//			jnt_vel_->setFromRad_s(j,
//					gazebo_joints_[joints_idx[j]]->GetVelocity(0));

			jnt_pos_->setFromRad(j,
					pos[j]);
			jnt_vel_->setFromRad_s(j, (pos[j] - jnt_pos_last->rad(j))/(rtt_time_-rtt_last_clock));


			jnt_trq_->setFromNm(j, friInst->getMsrJntTrq()[j]);
		}
		//		RTT::log(RTT::Error) << "\n\n" << RTT::endlog();


		switch (data_fs) {
		// Not Connected
		case RTT::NoData:
			set_brakes = true;

			break;

			// Connection lost
		case RTT::OldData:
			if (data_timestamp == last_data_timestamp && nb_no_data_++ >= 2){
				set_brakes = true;
			}
			break;

			// OK
		case RTT::NewData:
			set_brakes = false;
			if (nb_no_data_-- <= 0)
				nb_no_data_ = 0;
			break;
		}

		//		RTT::log(RTT::Error) << "Brakes?: " << set_brakes << RTT::endlog();

		// Copy Current joint pos in case of brakes
//		if (!set_brakes)
//			for (unsigned j = 0; j < joints_idx.size(); j++)
//				jnt_pos_brakes_->setFromRad(j, jnt_pos_->rad(j));

		// Force Joint Positions in case of a cmd
		if (set_new_pos) {
			RTT::log(RTT::Error) << "set_new_pos = true" << RTT::endlog();
			// Update specific joints regarding cmd
			float jnt_pos_robot[joints_idx.size()];
			for (unsigned j = 0; j < joints_idx.size(); j++) {
//				gazebo_joints_[joints_idx[j]]->SetPosition(0,
//						jnt_pos_cmd_->rad(j));
				jnt_pos_robot[j] = jnt_pos_cmd_->rad(j);

			}
			friInst->doPositionControl(jnt_pos_robot,false);
			// Aknowledge the settings
			set_new_pos = false;

		} else if (set_brakes) {
//			for (unsigned j = 0; j < joints_idx.size(); j++)
//				gazebo_joints_[joints_idx[j]]->SetPosition(0,
//						jnt_pos_brakes_->rad(j));
			friInst->doTest();
		} else {
			// Write command
			// Update specific joints regarding cmd
			float thau[joints_idx.size()];
			float q[joints_idx.size()];
			for (unsigned j = 0; j < joints_idx.size(); j++) {
//				gazebo_joints_[joints_idx[j]]->SetForce(0, jnt_trq_cmd_->Nm(j));
				//				RTT::log(RTT::Error) << "set Force: " << j << ", "
				//						<< jnt_trq_cmd_->Nm(j) << RTT::endlog();
				thau[j]=jnt_trq_cmd_->Nm(j)-friInst->getGrav()[j];
				q[j]=jnt_pos_->rad(j);
			}
			friInst->doJntImpedanceControl(q, NULL, NULL, thau);
		}
		friInst->doSendData();
		last_data_timestamp = data_timestamp;

	}

	virtual bool configureHook() {
		friInst = new friRemote(49939, "192.168.0.21");
		lastQuality = FRI_QUALITY_BAD;
		RTT::log(RTT::Info)<<"ARGH!\n"<<RTT::endlog();
		return true;
		//		last;
	}

	void updateData() {
//		if (port_JointPositionCommand.connected()
//				|| port_JointTorqueCommand.connected()
//				|| port_JointVelocityCommand.connected()) {

//		}

		static double last_update_time_sim;
		period_sim_ = rtt_time_ - last_update_time_sim;
		last_update_time_sim = rtt_time_;

		// Compute period in wall clock
		static double last_update_time_wall;
		period_wall_ = wall_time_ - last_update_time_wall;
		last_update_time_wall = wall_time_;

		// Increment simulation step counter (debugging)
		steps_rtt_++;

		// Get command from ports

		RTT::os::TimeService::ticks read_start =
				RTT::os::TimeService::Instance()->getTicks();

		data_fs = port_JointTorqueCommand.read(jnt_trq_cmd_);

		jnt_pos_fs = port_JointPositionCommand.read(jnt_pos_cmd_);

//		if (data_fs == RTT::NewData)
//			for (int i = 0; i < jnt_trq_cmd_->getDimension(); i++) {
//				l(RTT::Warning)<< "i: " << i << " = " << jnt_trq_cmd_->Nm(i) << RTT::endlog();
//			}

		if (jnt_pos_fs == RTT::NewData) {
			//set_new_pos = true; // TODO remove!
		}

		data_timestamp = new_pos_timestamp =
				RTT::os::TimeService::Instance()->getNSecs();

		read_duration_ = RTT::os::TimeService::Instance()->secondsSince(
				read_start);

		// Write state to ports
		RTT::os::TimeService::ticks write_start =
				RTT::os::TimeService::Instance()->getTicks();

		port_JointVelocity.write(jnt_vel_);
		port_JointPosition.write(jnt_pos_);
		port_JointTorque.write(jnt_trq_);

		write_duration_ = RTT::os::TimeService::Instance()->secondsSince(
				write_start);

		switch (data_fs) {
		case RTT::OldData:
			break;
		case RTT::NewData:
//			RTT::log(RTT::Error) << getName() << " " << data_fs << " at "
//					<< data_timestamp << RTT::endlog();
			nb_cmd_received_++;
			last_timestamp = data_timestamp;

//			for (int i = 0; i < jnt_trq_cmd_->getDimension(); i++) {
//				RTT::log(RTT::Info) << jnt_trq_cmd_->Nm(i) << RTT::endlog();
//			}
//			RTT::log(RTT::Info) << "-------" << RTT::endlog();
			break;
		case RTT::NoData:
			nb_cmd_received_ = 0;
			break;
		}
		RTT::log(RTT::Error)<<"tmp!\n"<<RTT::endlog();
	}

//	virtual void updateHook() {
//		return;
//	}
protected:

	//! Synchronization ??

	std::vector<int> joints_idx;

	std::map<gazebo::physics::LinkPtr, bool> gravity_mode_;

	std::vector<gazebo::physics::JointPtr> gazebo_joints_;
	gazebo::physics::Link_V model_links_;
	std::vector<std::string> joint_names_;

	RTT::InputPort<rci::JointAnglesPtr> port_JointPositionCommand;
	RTT::InputPort<rci::JointTorquesPtr> port_JointTorqueCommand;
	RTT::InputPort<rci::JointVelocitiesPtr> port_JointVelocityCommand;

	RTT::OutputPort<rci::JointAnglesPtr> port_JointPosition;
	RTT::OutputPort<rci::JointTorquesPtr> port_JointTorque;
	RTT::OutputPort<rci::JointVelocitiesPtr> port_JointVelocity;

	RTT::FlowStatus jnt_pos_fs, data_fs;
	rci::JointAnglesPtr jnt_pos_cmd_, jnt_pos_, jnt_pos_last;
	rci::JointTorquesPtr jnt_trq_, jnt_trq_cmd_;
	rci::JointVelocitiesPtr jnt_vel_, jnt_vel_cmd_;
	rci::JointAnglesPtr jnt_pos_brakes_;

	//! RTT time for debugging
	double rtt_time_;
	//! Gazebo time for debugging
	double gz_time_;
	double wall_time_;

	RTT::nsecs last_gz_update_time_, new_pos_timestamp;
	RTT::Seconds gz_period_;
	RTT::Seconds gz_duration_;

	RTT::nsecs last_update_time_;
	RTT::Seconds rtt_period_;
	RTT::Seconds read_duration_;
	RTT::Seconds write_duration_;

	int steps_gz_;
	int steps_rtt_, last_steps_rtt_, nb_no_data_;
//	unsigned int n_joints_;
	int cnt_lock_;
	double period_sim_;
	double period_wall_;
	boost::atomic<bool> new_data, set_new_pos;
	boost::atomic<bool> rtt_done, gazebo_done;

	RTT::nsecs last_data_timestamp, data_timestamp, last_timestamp;bool set_brakes;
	int nb_static_joints;

	int nb_cmd_received_;bool sync_with_cmds_;

	// contains the urdf string for the associated model.
	std::string urdf_string;


	//JOSH STUFF!!!
	friRemote* friInst;
	std::string ip_left;
	FRI_QUALITY lastQuality;
};

ORO_LIST_COMPONENT_TYPE(LWRFriComponent)
ORO_CREATE_COMPONENT_LIBRARY();

