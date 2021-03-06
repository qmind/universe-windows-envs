#ifndef ENV_H_
#define ENV_H_

#pragma comment(lib, "vJoyInterface")

#include "public.h"
#include "vjoyinterface.h"
#include "Rewarder.h"
#include "JoystickController.h"
#include <boost/algorithm/string/predicate.hpp>
#include <atomic>

const int kMaxResetWaitSecs = 10;

class Env
{
public:
	Env::Env(std::string env_id, std::string instance_id, int websocket_port, std::shared_ptr<AgentConn> agent_conn, boost::log::sources::severity_logger_mt<ls::severity_level> lg, int rewards_per_second = 60) :
		rewarder(websocket_port, env_id, agent_conn, rewards_per_second, lg),
		joystick_(), 
		reset_lock_(reset_mtx_, std::defer_lock)
	{
		env_id_ = env_id;
		agent_conn_ = agent_conn;
		agent_reset_connection  = agent_conn->on_reset      (boost::bind(&Env::reset,            this, boost::placeholders::_1));
		agent_action_connection = agent_conn->on_action     (boost::bind(&Env::_act,             this, boost::placeholders::_1));
		no_clients_connection   = agent_conn->on_no_clients (boost::bind(&Env::_when_no_clients, this));
		agent_conn_->send_env_describe(env_id_, "running", rewarder.get_episode_id(), rewarder.get_frames_per_second());
		lg_ = lg;
	}

	virtual Env::~Env()
	{
		// Close internal event-bus like connections managed by boost
		agent_reset_connection.disconnect();
		agent_action_connection.disconnect();
		no_clients_connection.disconnect();
	}

	bool resetting() const
	{
		return reset_lock_.owns_lock();
	};

	void reset(const Json::Value agent_request=Json::nullValue)
	{
		if (resetting())
		{
			// Skip concurrent reset requests
			BOOST_LOG_SEV(lg_, ls::warning) << "Concurrent: reset request, ignoring";
			return;
		}
		std::lock_guard<decltype(reset_lock_)> g(reset_lock_);
		before_reset();
		agent_conn_->send_reset_reply(agent_request, rewarder.get_episode_id());
		agent_conn_->send_env_describe(env_id_, "resetting", rewarder.get_episode_id(), rewarder.get_frames_per_second());
		reset_game();
		rewarder.reset();
		agent_conn_->send_env_describe(env_id_, "running", rewarder.get_episode_id(), rewarder.get_frames_per_second());
		after_reset();
	}

	void _when_no_clients()
	{
		rewarder.hard_reset();
		when_no_clients();
	}

	void _act(const Json::Value agent_request)
	{
		// unable to pass unimplemented virtual function directly to bind
		act(agent_request);
	}

	virtual void loop() = 0;
	virtual void connect() = 0; // connects to game
	virtual void step() = 0;
	virtual bool is_done() = 0;
	virtual void reset_game() = 0;
	virtual void before_reset() = 0;
	virtual void after_reset() = 0;
	virtual void when_no_clients()
	{
		// this can get called before binding to child object takes place, so can't be pure virtual (i think)
	};

	virtual void change_settings(const Json::Value& settings) = 0;

	// NB: Non-joystick actions go through Tight VNC
	void act(const Json::Value& agent_request)
	{
		Json::Value action = agent_request["body"]["action"];
		for (Json::Value event : action)
		{
			try
			{
				if (boost::starts_with(event[0].asString(), "Joystick"))
				{
					joystick_.set(event);
				}
				else if (boost::starts_with(event[0].asString(), "GameSettings"))
				{
					change_settings(event);
				}
			}
			catch (...)
			{
				BOOST_LOG_SEV(lg_, ls::error) << "Error processing act request" << std::endl << boost::current_exception_diagnostic_information();
			}
		}
	}

	Rewarder rewarder;
	boost::signals2::connection agent_reset_connection;
	boost::signals2::connection agent_action_connection;
	boost::signals2::connection no_clients_connection;
protected:
	JoystickController joystick_;
	boost::log::sources::severity_logger_mt<ls::severity_level> lg_;
	std::shared_ptr<AgentConn> agent_conn_;
	std::unique_lock<std::mutex> reset_lock_;
private:
	std::string env_id_;
	std::mutex reset_mtx_;
};

#endif // !ENV_H_