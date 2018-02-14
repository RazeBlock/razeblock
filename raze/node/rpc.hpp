#pragma once

#include <raze/node/utility.hpp>

//#include <boost/beast/http.hpp>
//#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast.hpp>

#include <boost/asio.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <atomic>
#include <unordered_map>

namespace raze
{
class node;
class rpc_config
{
public:
	rpc_config ();
	rpc_config (bool);
	void serialize_json (boost::property_tree::ptree &) const;
	bool deserialize_json (boost::property_tree::ptree const &);
	boost::asio::ip::address_v6 address;
	uint16_t port;
	bool enable_control;
	uint64_t frontier_request_limit;
	uint64_t chain_request_limit;
};
enum class payment_status
{
	not_a_status,
	unknown,
	nothing, // Timeout and nothing was received
	//insufficient, // Timeout and not enough was received
	//over, // More than requested received
	//success_fork, // Amount received but it involved a fork
	success // Amount received
};
class wallet;
class payment_observer;
class rpc
{
public:
	rpc (boost::asio::io_service &, raze::node &, raze::rpc_config const &);
	void start ();
	void stop ();
	void observer_action (raze::account const &);
	boost::asio::ip::tcp::acceptor acceptor;
	std::mutex mutex;
	std::unordered_map<raze::account, std::shared_ptr<raze::payment_observer>> payment_observers;
	raze::rpc_config config;
	raze::node & node;
	bool on;
	static uint16_t const rpc_port = raze::raze_network == raze::raze_networks::raze_live_network ? 7076 : 55000;
};
class rpc_connection : public std::enable_shared_from_this<raze::rpc_connection>
{
public:
	rpc_connection (raze::node &, raze::rpc &);
	void parse_connection ();
	std::shared_ptr<raze::node> node;
	raze::rpc & rpc;
	boost::asio::ip::tcp::socket socket;
	boost::beast::flat_buffer buffer;
	boost::beast::http::request<boost::beast::http::string_body> request;
	boost::beast::http::response<boost::beast::http::string_body> res;
};
class payment_observer : public std::enable_shared_from_this<raze::payment_observer>
{
public:
	payment_observer (std::function<void(boost::property_tree::ptree const &)> const &, raze::rpc &, raze::account const &, raze::amount const &);
	~payment_observer ();
	void start (uint64_t);
	void observe ();
	void timeout ();
	void complete (raze::payment_status);
	std::mutex mutex;
	std::condition_variable condition;
	raze::rpc & rpc;
	raze::account account;
	raze::amount amount;
	std::function<void(boost::property_tree::ptree const &)> response;
	std::atomic_flag completed;
};
class rpc_handler : public std::enable_shared_from_this<raze::rpc_handler>
{
public:
	rpc_handler (raze::node &, raze::rpc &, std::string const &, std::function<void(boost::property_tree::ptree const &)> const &);
	void process_request ();
	void account_balance ();
	void account_block_count ();
	void account_create ();
	void account_get ();
	void account_history ();
	void account_info ();
	void account_key ();
	void account_list ();
	void account_move ();
	void account_remove ();
	void account_representative ();
	void account_representative_set ();
	void account_weight ();
	void accounts_balances ();
	void accounts_create ();
	void accounts_frontiers ();
	void accounts_pending ();
	void available_supply ();
	void block ();
	void blocks ();
	void blocks_info ();
	void block_account ();
	void block_count ();
	void block_count_type ();
	void block_create ();
	void bootstrap ();
	void bootstrap_any ();
	void chain ();
	void delegators ();
	void delegators_count ();
	void deterministic_key ();
	void frontiers ();
	void frontier_count ();
	void history ();
	void keepalive ();
	void key_create ();
	void key_expand ();
	void kraze_to_raw ();
	void kraze_from_raw ();
	void ledger ();
	void mraze_to_raw ();
	void mraze_from_raw ();
	void password_change ();
	void password_enter ();
	void password_valid (bool wallet_locked);
	void payment_begin ();
	void payment_init ();
	void payment_end ();
	void payment_wait ();
	void peers ();
	void pending ();
	void pending_exists ();
	void process ();
	void raze_to_raw ();
	void raze_from_raw ();
	void receive ();
	void receive_minimum ();
	void receive_minimum_set ();
	void representatives ();
	void republish ();
	void search_pending ();
	void search_pending_all ();
	void send ();
	void stop ();
	void successors ();
	void unchecked ();
	void unchecked_clear ();
	void unchecked_get ();
	void unchecked_keys ();
	void validate_account_number ();
	void version ();
	void wallet_add ();
	void wallet_balance_total ();
	void wallet_balances ();
	void wallet_change_seed ();
	void wallet_contains ();
	void wallet_create ();
	void wallet_destroy ();
	void wallet_export ();
	void wallet_frontiers ();
	void wallet_key_valid ();
	void wallet_lock ();
	void wallet_pending ();
	void wallet_representative ();
	void wallet_representative_set ();
	void wallet_republish ();
	void wallet_work_get ();
	void work_generate ();
	void work_cancel ();
	void work_get ();
	void work_set ();
	void work_validate ();
	void work_peer_add ();
	void work_peers ();
	void work_peers_clear ();
	std::string body;
	raze::node & node;
	raze::rpc & rpc;
	boost::property_tree::ptree request;
	std::function<void(boost::property_tree::ptree const &)> response;
};
}
