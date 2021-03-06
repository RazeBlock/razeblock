#include <raze/node/node.hpp>

#include <raze/lib/interface.h>
#include <raze/node/common.hpp>
#include <raze/node/rpc.hpp>

#include <algorithm>
#include <future>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_set>

#include <boost/log/expressions.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <upnpcommands.h>

#include <ed25519-donna/ed25519.h>

double constexpr raze::node::price_max;
double constexpr raze::node::free_cutoff;
std::chrono::seconds constexpr raze::node::period;
std::chrono::seconds constexpr raze::node::cutoff;
std::chrono::minutes constexpr raze::node::backup_interval;
int constexpr raze::port_mapping::mapping_timeout;
int constexpr raze::port_mapping::check_timeout;
unsigned constexpr raze::active_transactions::announce_interval_ms;

raze::message_statistics::message_statistics () :
keepalive (0),
publish (0),
confirm_req (0),
confirm_ack (0)
{
}

raze::network::network (raze::node & node_a, uint16_t port) :
socket (node_a.service, raze::endpoint (boost::asio::ip::address_v6::any (), port)),
resolver (node_a.service),
node (node_a),
bad_sender_count (0),
on (true),
insufficient_work_count (0),
error_count (0)
{
}

void raze::network::receive ()
{
	if (node.config.logging.network_packet_logging ())
	{
		BOOST_LOG (node.log) << "Receiving packet";
	}
	std::unique_lock<std::mutex> lock (socket_mutex);
	socket.async_receive_from (boost::asio::buffer (buffer.data (), buffer.size ()), remote, [this](boost::system::error_code const & error, size_t size_a) {
		receive_action (error, size_a);
	});
}

void raze::network::stop ()
{
	on = false;
	socket.close ();
	resolver.cancel ();
}

void raze::network::send_keepalive (raze::endpoint const & endpoint_a)
{
	assert (endpoint_a.address ().is_v6 ());
	raze::keepalive message;
	node.peers.random_fill (message.peers);
	std::shared_ptr<std::vector<uint8_t>> bytes (new std::vector<uint8_t>);
	{
		raze::vectorstream stream (*bytes);
		message.serialize (stream);
	}
	if (node.config.logging.network_keepalive_logging ())
	{
		BOOST_LOG (node.log) << boost::str (boost::format ("Keepalive req sent to %1%") % endpoint_a);
	}
	++outgoing.keepalive;
	std::weak_ptr<raze::node> node_w (node.shared ());
	send_buffer (bytes->data (), bytes->size (), endpoint_a, [bytes, node_w, endpoint_a](boost::system::error_code const & ec, size_t) {
		if (auto node_l = node_w.lock ())
		{
			if (ec && node_l->config.logging.network_keepalive_logging ())
			{
				BOOST_LOG (node_l->log) << boost::str (boost::format ("Error sending keepalive to %1% %2%") % endpoint_a % ec.message ());
			}
		}
	});
}

void raze::node::keepalive (std::string const & address_a, uint16_t port_a)
{
	auto node_l (shared_from_this ());
	network.resolver.async_resolve (boost::asio::ip::udp::resolver::query (address_a, std::to_string (port_a)), [node_l, address_a, port_a](boost::system::error_code const & ec, boost::asio::ip::udp::resolver::iterator i_a) {
		if (!ec)
		{
			for (auto i (i_a), n (boost::asio::ip::udp::resolver::iterator{}); i != n; ++i)
			{
				auto endpoint (i->endpoint ());
				if (endpoint.address ().is_v4 ())
				{
					endpoint = raze::endpoint (boost::asio::ip::address_v6::v4_mapped (endpoint.address ().to_v4 ()), endpoint.port ());
				}
				node_l->send_keepalive (endpoint);
			}
		}
		else
		{
			BOOST_LOG (node_l->log) << boost::str (boost::format ("Error resolving address: %1%:%2%, %3%") % address_a % port_a % ec.message ());
		}
	});
}

void raze::network::republish (raze::block_hash const & hash_a, std::shared_ptr<std::vector<uint8_t>> buffer_a, raze::endpoint endpoint_a)
{
	++outgoing.publish;
	if (node.config.logging.network_publish_logging ())
	{
		BOOST_LOG (node.log) << boost::str (boost::format ("Publishing %1% to %2%") % hash_a.to_string () % endpoint_a);
	}
	std::weak_ptr<raze::node> node_w (node.shared ());
	send_buffer (buffer_a->data (), buffer_a->size (), endpoint_a, [buffer_a, node_w, endpoint_a](boost::system::error_code const & ec, size_t size) {
		if (auto node_l = node_w.lock ())
		{
			if (ec && node_l->config.logging.network_logging ())
			{
				BOOST_LOG (node_l->log) << boost::str (boost::format ("Error sending publish: %1% to %2%") % ec.message () % endpoint_a);
			}
		}
	});
}

void raze::network::rebroadcast_reps (std::shared_ptr<raze::block> block_a)
{
	auto hash (block_a->hash ());
	raze::publish message (block_a);
	std::shared_ptr<std::vector<uint8_t>> bytes (new std::vector<uint8_t>);
	{
		raze::vectorstream stream (*bytes);
		message.serialize (stream);
	}
	auto representatives (node.peers.representatives (2 * node.peers.size_sqrt ()));
	for (auto i : representatives)
	{
		republish (hash, bytes, i.endpoint);
	}
}

template <typename T>
bool confirm_block (MDB_txn * transaction_a, raze::node & node_a, T & list_a, std::shared_ptr<raze::block> block_a)
{
	bool result (false);
	if (node_a.config.enable_voting)
	{
		node_a.wallets.foreach_representative (transaction_a, [&result, &block_a, &list_a, &node_a, &transaction_a](raze::public_key const & pub_a, raze::raw_key const & prv_a) {
			result = true;
			auto votes (node_a.store.vote_generate (transaction_a, pub_a, prv_a, block_a));
			for (auto vote : { votes.first, votes.second })
			{
				raze::confirm_ack confirm (vote);
				std::shared_ptr<std::vector<uint8_t>> bytes (new std::vector<uint8_t>);
				{
					raze::vectorstream stream (*bytes);
					confirm.serialize (stream);
				}
				for (auto j (list_a.begin ()), m (list_a.end ()); j != m; ++j)
				{
					node_a.network.confirm_send (confirm, bytes, *j);
				}
			}
		});
	}
	return result;
}

template <>
bool confirm_block (MDB_txn * transaction_a, raze::node & node_a, raze::endpoint & peer_a, std::shared_ptr<raze::block> block_a)
{
	std::array<raze::endpoint, 1> endpoints;
	endpoints[0] = peer_a;
	auto result (confirm_block (transaction_a, node_a, endpoints, std::move (block_a)));
	return result;
}

void raze::network::republish_block (MDB_txn * transaction, std::shared_ptr<raze::block> block)
{
	auto hash (block->hash ());
	auto list (node.peers.list_sqrt ());
	// If we're a representative, broadcast a signed confirm, otherwise an unsigned publish
	if (!confirm_block (transaction, node, list, block))
	{
		raze::publish message (block);
		std::shared_ptr<std::vector<uint8_t>> bytes (new std::vector<uint8_t>);
		{
			raze::vectorstream stream (*bytes);
			message.serialize (stream);
		}
		auto hash (block->hash ());
		for (auto i (list.begin ()), n (list.end ()); i != n; ++i)
		{
			republish (hash, bytes, *i);
		}
		if (node.config.logging.network_logging ())
		{
			BOOST_LOG (node.log) << boost::str (boost::format ("Block %1% was republished to peers") % hash.to_string ());
		}
	}
	else
	{
		if (node.config.logging.network_logging ())
		{
			BOOST_LOG (node.log) << boost::str (boost::format ("Block %1% was confirmed to peers") % hash.to_string ());
		}
	}
}

// In order to rate limit network traffic we republish:
// 1) Only if they are a non-replay vote of a block that's actively settling. Settling blocks are limited by block PoW
// 2) Only if a vote for this block hasn't been received in the previous X second.  This prevents rapid publishing of votes with increasing sequence numbers.
// 3) The rep has a weight > Y to prevent creating a lot of small-weight accounts to send out votes
void raze::network::republish_vote (std::chrono::steady_clock::time_point const & last_vote, std::shared_ptr<raze::vote> vote_a)
{
	if (last_vote < std::chrono::steady_clock::now () - std::chrono::seconds (1))
	{
		if (node.weight (vote_a->account) > raze::Mraze_ratio * 256)
		{
			raze::confirm_ack confirm (vote_a);
			std::shared_ptr<std::vector<uint8_t>> bytes (new std::vector<uint8_t>);
			{
				raze::vectorstream stream (*bytes);
				confirm.serialize (stream);
			}
			auto list (node.peers.list_sqrt ());
			for (auto j (list.begin ()), m (list.end ()); j != m; ++j)
			{
				node.network.confirm_send (confirm, bytes, *j);
			}
		}
	}
}

void raze::network::broadcast_confirm_req (std::shared_ptr<raze::block> block_a)
{
	auto list (node.peers.representatives (std::numeric_limits<size_t>::max ()));
	for (auto i (list.begin ()), j (list.end ()); i != j; ++i)
	{
		node.network.send_confirm_req (i->endpoint, block_a);
	}
	if (node.config.logging.network_logging ())
	{
		BOOST_LOG (node.log) << boost::str (boost::format ("Broadcasted confirm req to %1% representatives") % list.size ());
	}
}

void raze::network::send_confirm_req (raze::endpoint const & endpoint_a, std::shared_ptr<raze::block> block)
{
	raze::confirm_req message (block);
	std::shared_ptr<std::vector<uint8_t>> bytes (new std::vector<uint8_t>);
	{
		raze::vectorstream stream (*bytes);
		message.serialize (stream);
	}
	if (node.config.logging.network_message_logging ())
	{
		BOOST_LOG (node.log) << boost::str (boost::format ("Sending confirm req to %1%") % endpoint_a);
	}
	std::weak_ptr<raze::node> node_w (node.shared ());
	++outgoing.confirm_req;
	send_buffer (bytes->data (), bytes->size (), endpoint_a, [bytes, node_w](boost::system::error_code const & ec, size_t size) {
		if (auto node_l = node_w.lock ())
		{
			if (ec && node_l->config.logging.network_logging ())
			{
				BOOST_LOG (node_l->log) << boost::str (boost::format ("Error sending confirm request: %1%") % ec.message ());
			}
		}
	});
}

template <typename T>
void rep_query (raze::node & node_a, T const & peers_a)
{
	raze::transaction transaction (node_a.store.environment, nullptr, false);
	std::shared_ptr<raze::block> block (node_a.store.block_random (transaction));
	auto hash (block->hash ());
	node_a.rep_crawler.add (hash);
	for (auto i (peers_a.begin ()), n (peers_a.end ()); i != n; ++i)
	{
		node_a.peers.rep_request (*i);
		node_a.network.send_confirm_req (*i, block);
	}
	std::weak_ptr<raze::node> node_w (node_a.shared ());
	node_a.alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (5), [node_w, hash]() {
		if (auto node_l = node_w.lock ())
		{
			node_l->rep_crawler.remove (hash);
		}
	});
}

template <>
void rep_query (raze::node & node_a, raze::endpoint const & peers_a)
{
	std::array<raze::endpoint, 1> peers;
	peers[0] = peers_a;
	rep_query (node_a, peers);
}

namespace
{
class network_message_visitor : public raze::message_visitor
{
public:
	network_message_visitor (raze::node & node_a, raze::endpoint const & sender_a) :
	node (node_a),
	sender (sender_a)
	{
	}
	virtual ~network_message_visitor () = default;
	void keepalive (raze::keepalive const & message_a) override
	{
		if (node.config.logging.network_keepalive_logging ())
		{
			BOOST_LOG (node.log) << boost::str (boost::format ("Received keepalive message from %1%") % sender);
		}
		++node.network.incoming.keepalive;
		node.peers.contacted (sender, message_a.version_using);
		node.network.merge_peers (message_a.peers);
	}
	void publish (raze::publish const & message_a) override
	{
		if (node.config.logging.network_message_logging ())
		{
			BOOST_LOG (node.log) << boost::str (boost::format ("Publish message from %1% for %2%") % sender % message_a.block->hash ().to_string ());
		}
		++node.network.incoming.publish;
		node.peers.contacted (sender, message_a.version_using);
		node.peers.insert (sender, message_a.version_using);
		node.process_active (message_a.block);
	}
	void confirm_req (raze::confirm_req const & message_a) override
	{
		if (node.config.logging.network_message_logging ())
		{
			BOOST_LOG (node.log) << boost::str (boost::format ("Confirm_req message from %1% for %2%") % sender % message_a.block->hash ().to_string ());
		}
		++node.network.incoming.confirm_req;
		node.peers.contacted (sender, message_a.version_using);
		node.peers.insert (sender, message_a.version_using);
		node.process_active (message_a.block);
		raze::transaction transaction_a (node.store.environment, nullptr, false);
		if (node.store.block_exists (transaction_a, message_a.block->hash ()))
		{
			confirm_block (transaction_a, node, sender, message_a.block);
		}
	}
	void confirm_ack (raze::confirm_ack const & message_a) override
	{
		if (node.config.logging.network_message_logging ())
		{
			BOOST_LOG (node.log) << boost::str (boost::format ("Received confirm_ack message from %1% for %2% sequence %3%") % sender % message_a.vote->block->hash ().to_string () % std::to_string (message_a.vote->sequence));
		}
		++node.network.incoming.confirm_ack;
		node.peers.contacted (sender, message_a.version_using);
		node.peers.insert (sender, message_a.version_using);
		node.process_active (message_a.vote->block);
		auto vote (node.vote_processor.vote (message_a.vote, sender));
		if (vote.code == raze::vote_code::replay)
		{
			assert (vote.vote->sequence > message_a.vote->sequence);
			// This tries to assist rep nodes that have lost track of their highest sequence number by replaying our highest known vote back to them
			// Only do this if the sequence number is significantly different to account for network reordering
			// Amplify attack considerations: We're sending out a confirm_ack in response to a confirm_ack for no net traffic increase
			if (vote.vote->sequence - message_a.vote->sequence > 10000)
			{
				raze::confirm_ack confirm (vote.vote);
				std::shared_ptr<std::vector<uint8_t>> bytes (new std::vector<uint8_t>);
				{
					raze::vectorstream stream (*bytes);
					confirm.serialize (stream);
				}
				node.network.confirm_send (confirm, bytes, sender);
			}
		}
	}
	void bulk_pull (raze::bulk_pull const &) override
	{
		assert (false);
	}
	void bulk_pull_blocks (raze::bulk_pull_blocks const &) override
	{
		assert (false);
	}
	void bulk_push (raze::bulk_push const &) override
	{
		assert (false);
	}
	void frontier_req (raze::frontier_req const &) override
	{
		assert (false);
	}
	raze::node & node;
	raze::endpoint sender;
};
}

void raze::network::receive_action (boost::system::error_code const & error, size_t size_a)
{
	if (!error && on)
	{
		if (!raze::reserved_address (remote) && remote != endpoint ())
		{
			network_message_visitor visitor (node, remote);
			raze::message_parser parser (visitor, node.work);
			parser.deserialize_buffer (buffer.data (), size_a);
			if (parser.error)
			{
				++error_count;
			}
			else if (parser.insufficient_work)
			{
				if (node.config.logging.insufficient_work_logging ())
				{
					BOOST_LOG (node.log) << "Insufficient work in message";
				}
				++insufficient_work_count;
			}
		}
		else
		{
			if (node.config.logging.network_logging ())
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("Reserved sender %1%") % remote.address ().to_string ());
			}
			++bad_sender_count;
		}
		receive ();
	}
	else
	{
		if (error)
		{
			if (node.config.logging.network_logging ())
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("UDP Receive error: %1%") % error.message ());
			}
		}
		if (on)
		{
			node.alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (5), [this]() { receive (); });
		}
	}
}

// Send keepalives to all the peers we've been notified of
void raze::network::merge_peers (std::array<raze::endpoint, 8> const & peers_a)
{
	for (auto i (peers_a.begin ()), j (peers_a.end ()); i != j; ++i)
	{
		if (!node.peers.reachout (*i))
		{
			send_keepalive (*i);
		}
	}
}

bool raze::operation::operator> (raze::operation const & other_a) const
{
	return wakeup > other_a.wakeup;
}

raze::alarm::alarm (boost::asio::io_service & service_a) :
service (service_a),
thread ([this]() { run (); })
{
}

raze::alarm::~alarm ()
{
	add (std::chrono::steady_clock::now (), nullptr);
	thread.join ();
}

void raze::alarm::run ()
{
	std::unique_lock<std::mutex> lock (mutex);
	auto done (false);
	while (!done)
	{
		if (!operations.empty ())
		{
			auto & operation (operations.top ());
			if (operation.function)
			{
				if (operation.wakeup <= std::chrono::steady_clock::now ())
				{
					service.post (operation.function);
					operations.pop ();
				}
				else
				{
					auto wakeup (operation.wakeup);
					condition.wait_until (lock, wakeup);
				}
			}
			else
			{
				done = true;
			}
		}
		else
		{
			condition.wait (lock);
		}
	}
}

void raze::alarm::add (std::chrono::steady_clock::time_point const & wakeup_a, std::function<void()> const & operation)
{
	std::lock_guard<std::mutex> lock (mutex);
	operations.push (raze::operation ({ wakeup_a, operation }));
	condition.notify_all ();
}

raze::logging::logging () :
ledger_logging_value (false),
ledger_duplicate_logging_value (false),
vote_logging_value (false),
network_logging_value (true),
network_message_logging_value (false),
network_publish_logging_value (false),
network_packet_logging_value (false),
network_keepalive_logging_value (false),
node_lifetime_tracing_value (false),
insufficient_work_logging_value (true),
log_rpc_value (true),
bulk_pull_logging_value (false),
work_generation_time_value (true),
log_to_cerr_value (false),
max_size (16 * 1024 * 1024),
rotation_size (4 * 1024 * 1024),
flush (true)
{
}

void raze::logging::init (boost::filesystem::path const & application_path_a)
{
	static std::atomic_flag logging_already_added = ATOMIC_FLAG_INIT;
	if (!logging_already_added.test_and_set ())
	{
		boost::log::add_common_attributes ();
		if (log_to_cerr ())
		{
			boost::log::add_console_log (std::cerr, boost::log::keywords::format = "[%TimeStamp%]: %Message%");
		}
		boost::log::add_file_log (boost::log::keywords::target = application_path_a / "log", boost::log::keywords::file_name = application_path_a / "log" / "log_%Y-%m-%d_%H-%M-%S.%N.log", boost::log::keywords::rotation_size = rotation_size, boost::log::keywords::auto_flush = flush, boost::log::keywords::scan_method = boost::log::sinks::file::scan_method::scan_matching, boost::log::keywords::max_size = max_size, boost::log::keywords::format = "[%TimeStamp%]: %Message%");
	}
}

void raze::logging::serialize_json (boost::property_tree::ptree & tree_a) const
{
	tree_a.put ("version", "3");
	tree_a.put ("ledger", ledger_logging_value);
	tree_a.put ("ledger_duplicate", ledger_duplicate_logging_value);
	tree_a.put ("vote", vote_logging_value);
	tree_a.put ("network", network_logging_value);
	tree_a.put ("network_message", network_message_logging_value);
	tree_a.put ("network_publish", network_publish_logging_value);
	tree_a.put ("network_packet", network_packet_logging_value);
	tree_a.put ("network_keepalive", network_keepalive_logging_value);
	tree_a.put ("node_lifetime_tracing", node_lifetime_tracing_value);
	tree_a.put ("insufficient_work", insufficient_work_logging_value);
	tree_a.put ("log_rpc", log_rpc_value);
	tree_a.put ("bulk_pull", bulk_pull_logging_value);
	tree_a.put ("work_generation_time", work_generation_time_value);
	tree_a.put ("log_to_cerr", log_to_cerr_value);
	tree_a.put ("max_size", max_size);
	tree_a.put ("rotation_size", rotation_size);
	tree_a.put ("flush", flush);
}

bool raze::logging::upgrade_json (unsigned version_a, boost::property_tree::ptree & tree_a)
{
	auto result (false);
	switch (version_a)
	{
		case 1:
			tree_a.put ("vote", vote_logging_value);
			tree_a.put ("version", "2");
			result = true;
		case 2:
			tree_a.put ("rotation_size", "4194304");
			tree_a.put ("flush", "true");
			tree_a.put ("version", "3");
			result = true;
		case 3:
			break;
		default:
			throw std::runtime_error ("Unknown logging_config version");
			break;
	}
	return result;
}

bool raze::logging::deserialize_json (bool & upgraded_a, boost::property_tree::ptree & tree_a)
{
	auto result (false);
	try
	{
		auto version_l (tree_a.get_optional<std::string> ("version"));
		if (!version_l)
		{
			tree_a.put ("version", "1");
			version_l = "1";
			auto work_peers_l (tree_a.get_child_optional ("work_peers"));
			if (!work_peers_l)
			{
				tree_a.add_child ("work_peers", boost::property_tree::ptree ());
			}
			upgraded_a = true;
		}
		upgraded_a |= upgrade_json (std::stoull (version_l.get ()), tree_a);
		ledger_logging_value = tree_a.get<bool> ("ledger");
		ledger_duplicate_logging_value = tree_a.get<bool> ("ledger_duplicate");
		vote_logging_value = tree_a.get<bool> ("vote");
		network_logging_value = tree_a.get<bool> ("network");
		network_message_logging_value = tree_a.get<bool> ("network_message");
		network_publish_logging_value = tree_a.get<bool> ("network_publish");
		network_packet_logging_value = tree_a.get<bool> ("network_packet");
		network_keepalive_logging_value = tree_a.get<bool> ("network_keepalive");
		node_lifetime_tracing_value = tree_a.get<bool> ("node_lifetime_tracing");
		insufficient_work_logging_value = tree_a.get<bool> ("insufficient_work");
		log_rpc_value = tree_a.get<bool> ("log_rpc");
		bulk_pull_logging_value = tree_a.get<bool> ("bulk_pull");
		work_generation_time_value = tree_a.get<bool> ("work_generation_time");
		log_to_cerr_value = tree_a.get<bool> ("log_to_cerr");
		max_size = tree_a.get<uintmax_t> ("max_size");
		rotation_size = tree_a.get<uintmax_t> ("rotation_size", 4194304);
		flush = tree_a.get<bool> ("flush", true);
	}
	catch (std::runtime_error const &)
	{
		result = true;
	}
	return result;
}

bool raze::logging::ledger_logging () const
{
	return ledger_logging_value;
}

bool raze::logging::ledger_duplicate_logging () const
{
	return ledger_logging () && ledger_duplicate_logging_value;
}

bool raze::logging::vote_logging () const
{
	return vote_logging_value;
}

bool raze::logging::network_logging () const
{
	return network_logging_value;
}

bool raze::logging::network_message_logging () const
{
	return network_logging () && network_message_logging_value;
}

bool raze::logging::network_publish_logging () const
{
	return network_logging () && network_publish_logging_value;
}

bool raze::logging::network_packet_logging () const
{
	return network_logging () && network_packet_logging_value;
}

bool raze::logging::network_keepalive_logging () const
{
	return network_logging () && network_keepalive_logging_value;
}

bool raze::logging::node_lifetime_tracing () const
{
	return node_lifetime_tracing_value;
}

bool raze::logging::insufficient_work_logging () const
{
	return network_logging () && insufficient_work_logging_value;
}

bool raze::logging::log_rpc () const
{
	return network_logging () && log_rpc_value;
}

bool raze::logging::bulk_pull_logging () const
{
	return network_logging () && bulk_pull_logging_value;
}

bool raze::logging::callback_logging () const
{
	return network_logging ();
}

bool raze::logging::work_generation_time () const
{
	return work_generation_time_value;
}

bool raze::logging::log_to_cerr () const
{
	return log_to_cerr_value;
}

raze::node_init::node_init () :
block_store_init (false),
wallet_init (false)
{
}

bool raze::node_init::error ()
{
	return block_store_init || wallet_init;
}

raze::node_config::node_config () :
node_config (raze::network::node_port, raze::logging ())
{
}

raze::node_config::node_config (uint16_t peering_port_a, raze::logging const & logging_a) :
peering_port (peering_port_a),
logging (logging_a),
bootstrap_fraction_numerator (1),
receive_minimum (raze::raze_ratio),
inactive_supply (0),
password_fanout (1024),
io_threads (std::max<unsigned> (4, std::thread::hardware_concurrency ())),
work_threads (std::max<unsigned> (4, std::thread::hardware_concurrency ())),
enable_voting (true),
bootstrap_connections (4),
bootstrap_connections_max (64),
callback_port (0),
lmdb_max_dbs (128)
{
	switch (raze::raze_network)
	{
		case raze::raze_networks::raze_test_network:
			preconfigured_representatives.push_back (raze::genesis_account);
			break;
		case raze::raze_networks::raze_beta_network:
			preconfigured_peers.push_back ("raze.raze.net");
			preconfigured_representatives.push_back (raze::account ("59750C057F42806F40C5D9EAA1E0263E9DB48FE385BD0172BFC573BD37EEC4A7"));
			preconfigured_representatives.push_back (raze::account ("8B05C9B160DE9B006FA27DD6A368D7CA122A2EE7537C308CF22EFD3ABF5B36C3"));
			preconfigured_representatives.push_back (raze::account ("91D51BF05F02698EBB4649FB06D1BBFD2E4AE2579660E8D784A002D9C0CB1BD2"));
			preconfigured_representatives.push_back (raze::account ("CB35ED23D47E1A16667EDE415CD4CD05961481D7D23A43958FAE81FC12FA49FF"));
			break;
		case raze::raze_networks::raze_live_network:
			preconfigured_peers.push_back ("peer.razeblock.com");
			preconfigured_representatives.push_back (raze::account ("A79F9116B2DBFF6EFCF5C9AA54414DD0F3B9751D5764D5EFAB255C78CB6103E2"));
			preconfigured_representatives.push_back (raze::account ("D1FD30E385727B74A88FD7DEBE166CEF3E8C4022EC2E83614B1B7F81D46D1EDF"));
			preconfigured_representatives.push_back (raze::account ("4FAA2923A361D6C5EC7C24A169E36828A99DEA01C2C1706F84FA6A21CFE63204"));
			preconfigured_representatives.push_back (raze::account ("198D9F4957508AC00A8D5EE730DF2D8951D190390FA359CF1E6C5E63364DBB9B"));
			preconfigured_representatives.push_back (raze::account ("3CE02C13C42F75F7B90A04660A18109AFBFD6E84B0A7A5DEBED9E1455ED032F5"));
			preconfigured_representatives.push_back (raze::account ("0148A669489E502CF047754318229784BF8E3E0E54F2E0E6976C9C961E0A81B0"));
			preconfigured_representatives.push_back (raze::account ("D772B5D5AE499935E78DE971C843BAD466C914B0899F30DD94B072672E8A0639"));
			preconfigured_representatives.push_back (raze::account ("5B620353D34F3E461A10B9FFA814AB35FBA7A30F3F04DBBB1A83CA9C092AEB0C"));
			break;
		default:
			assert (false);
			break;
	}
}

void raze::node_config::serialize_json (boost::property_tree::ptree & tree_a) const
{
	tree_a.put ("version", "9");
	tree_a.put ("peering_port", std::to_string (peering_port));
	tree_a.put ("bootstrap_fraction_numerator", std::to_string (bootstrap_fraction_numerator));
	tree_a.put ("receive_minimum", receive_minimum.to_string_dec ());
	boost::property_tree::ptree logging_l;
	logging.serialize_json (logging_l);
	tree_a.add_child ("logging", logging_l);
	boost::property_tree::ptree work_peers_l;
	for (auto i (work_peers.begin ()), n (work_peers.end ()); i != n; ++i)
	{
		boost::property_tree::ptree entry;
		entry.put ("", boost::str (boost::format ("%1%:%2%") % i->first % i->second));
		work_peers_l.push_back (std::make_pair ("", entry));
	}
	tree_a.add_child ("work_peers", work_peers_l);
	boost::property_tree::ptree preconfigured_peers_l;
	for (auto i (preconfigured_peers.begin ()), n (preconfigured_peers.end ()); i != n; ++i)
	{
		boost::property_tree::ptree entry;
		entry.put ("", *i);
		preconfigured_peers_l.push_back (std::make_pair ("", entry));
	}
	tree_a.add_child ("preconfigured_peers", preconfigured_peers_l);
	boost::property_tree::ptree preconfigured_representatives_l;
	for (auto i (preconfigured_representatives.begin ()), n (preconfigured_representatives.end ()); i != n; ++i)
	{
		boost::property_tree::ptree entry;
		entry.put ("", i->to_account ());
		preconfigured_representatives_l.push_back (std::make_pair ("", entry));
	}
	tree_a.add_child ("preconfigured_representatives", preconfigured_representatives_l);
	tree_a.put ("inactive_supply", inactive_supply.to_string_dec ());
	tree_a.put ("password_fanout", std::to_string (password_fanout));
	tree_a.put ("io_threads", std::to_string (io_threads));
	tree_a.put ("work_threads", std::to_string (work_threads));
	tree_a.put ("enable_voting", enable_voting);
	tree_a.put ("bootstrap_connections", bootstrap_connections);
	tree_a.put ("bootstrap_connections_max", bootstrap_connections_max);
	tree_a.put ("callback_address", callback_address);
	tree_a.put ("callback_port", std::to_string (callback_port));
	tree_a.put ("callback_target", callback_target);
	tree_a.put ("lmdb_max_dbs", lmdb_max_dbs);
}

bool raze::node_config::upgrade_json (unsigned version, boost::property_tree::ptree & tree_a)
{
	auto result (false);
	switch (version)
	{
		case 1:
		{
			auto reps_l (tree_a.get_child ("preconfigured_representatives"));
			boost::property_tree::ptree reps;
			for (auto i (reps_l.begin ()), n (reps_l.end ()); i != n; ++i)
			{
				raze::uint256_union account;
				account.decode_account (i->second.get<std::string> (""));
				boost::property_tree::ptree entry;
				entry.put ("", account.to_account ());
				reps.push_back (std::make_pair ("", entry));
			}
			tree_a.erase ("preconfigured_representatives");
			tree_a.add_child ("preconfigured_representatives", reps);
			tree_a.erase ("version");
			tree_a.put ("version", "2");
			result = true;
		}
		case 2:
		{
			tree_a.put ("inactive_supply", raze::uint128_union (0).to_string_dec ());
			tree_a.put ("password_fanout", std::to_string (1024));
			tree_a.put ("io_threads", std::to_string (io_threads));
			tree_a.put ("work_threads", std::to_string (work_threads));
			tree_a.erase ("version");
			tree_a.put ("version", "3");
			result = true;
		}
		case 3:
			tree_a.erase ("receive_minimum");
			tree_a.put ("receive_minimum", raze::raze_ratio.convert_to<std::string> ());
			tree_a.erase ("version");
			tree_a.put ("version", "4");
			result = true;
		case 4:
			tree_a.erase ("receive_minimum");
			tree_a.put ("receive_minimum", raze::raze_ratio.convert_to<std::string> ());
			tree_a.erase ("version");
			tree_a.put ("version", "5");
			result = true;
		case 5:
			tree_a.put ("enable_voting", enable_voting);
			tree_a.erase ("packet_delay_microseconds");
			tree_a.erase ("rebroadcast_delay");
			tree_a.erase ("creation_rebroadcast");
			tree_a.erase ("version");
			tree_a.put ("version", "6");
			result = true;
		case 6:
			tree_a.put ("bootstrap_connections", 16);
			tree_a.put ("callback_address", "");
			tree_a.put ("callback_port", "0");
			tree_a.put ("callback_target", "");
			tree_a.erase ("version");
			tree_a.put ("version", "7");
			result = true;
		case 7:
			tree_a.put ("lmdb_max_dbs", "128");
			tree_a.erase ("version");
			tree_a.put ("version", "8");
			result = true;
		case 8:
			tree_a.put ("bootstrap_connections_max", "64");
			tree_a.erase ("version");
			tree_a.put ("version", "9");
			result = true;
		case 9:
			break;
		default:
			throw std::runtime_error ("Unknown node_config version");
	}
	return result;
}

bool raze::node_config::deserialize_json (bool & upgraded_a, boost::property_tree::ptree & tree_a)
{
	auto result (false);
	try
	{
		auto version_l (tree_a.get_optional<std::string> ("version"));
		if (!version_l)
		{
			tree_a.put ("version", "1");
			version_l = "1";
			auto work_peers_l (tree_a.get_child_optional ("work_peers"));
			if (!work_peers_l)
			{
				tree_a.add_child ("work_peers", boost::property_tree::ptree ());
			}
			upgraded_a = true;
		}
		upgraded_a |= upgrade_json (std::stoull (version_l.get ()), tree_a);
		auto peering_port_l (tree_a.get<std::string> ("peering_port"));
		auto bootstrap_fraction_numerator_l (tree_a.get<std::string> ("bootstrap_fraction_numerator"));
		auto receive_minimum_l (tree_a.get<std::string> ("receive_minimum"));
		auto & logging_l (tree_a.get_child ("logging"));
		work_peers.clear ();
		auto work_peers_l (tree_a.get_child ("work_peers"));
		for (auto i (work_peers_l.begin ()), n (work_peers_l.end ()); i != n; ++i)
		{
			auto work_peer (i->second.get<std::string> (""));
			boost::asio::ip::address address;
			uint16_t port;
			result |= raze::parse_address_port (work_peer, address, port);
			if (!result)
			{
				work_peers.push_back (std::make_pair (address, port));
			}
		}
		auto preconfigured_peers_l (tree_a.get_child ("preconfigured_peers"));
		preconfigured_peers.clear ();
		for (auto i (preconfigured_peers_l.begin ()), n (preconfigured_peers_l.end ()); i != n; ++i)
		{
			auto bootstrap_peer (i->second.get<std::string> (""));
			preconfigured_peers.push_back (bootstrap_peer);
		}
		auto preconfigured_representatives_l (tree_a.get_child ("preconfigured_representatives"));
		preconfigured_representatives.clear ();
		for (auto i (preconfigured_representatives_l.begin ()), n (preconfigured_representatives_l.end ()); i != n; ++i)
		{
			raze::account representative (0);
			result = result || representative.decode_account (i->second.get<std::string> (""));
			preconfigured_representatives.push_back (representative);
		}
		if (preconfigured_representatives.empty ())
		{
			result = true;
		}
		auto inactive_supply_l (tree_a.get<std::string> ("inactive_supply"));
		auto password_fanout_l (tree_a.get<std::string> ("password_fanout"));
		auto io_threads_l (tree_a.get<std::string> ("io_threads"));
		auto work_threads_l (tree_a.get<std::string> ("work_threads"));
		enable_voting = tree_a.get<bool> ("enable_voting");
		auto bootstrap_connections_l (tree_a.get<std::string> ("bootstrap_connections"));
		auto bootstrap_connections_max_l (tree_a.get<std::string> ("bootstrap_connections_max"));
		callback_address = tree_a.get<std::string> ("callback_address");
		auto callback_port_l (tree_a.get<std::string> ("callback_port"));
		callback_target = tree_a.get<std::string> ("callback_target");
		auto lmdb_max_dbs_l = tree_a.get<std::string> ("lmdb_max_dbs");
		result |= parse_port (callback_port_l, callback_port);
		try
		{
			peering_port = std::stoul (peering_port_l);
			bootstrap_fraction_numerator = std::stoul (bootstrap_fraction_numerator_l);
			password_fanout = std::stoul (password_fanout_l);
			io_threads = std::stoul (io_threads_l);
			work_threads = std::stoul (work_threads_l);
			bootstrap_connections = std::stoul (bootstrap_connections_l);
			bootstrap_connections_max = std::stoul (bootstrap_connections_max_l);
			lmdb_max_dbs = std::stoi (lmdb_max_dbs_l);
			result |= peering_port > std::numeric_limits<uint16_t>::max ();
			result |= logging.deserialize_json (upgraded_a, logging_l);
			result |= receive_minimum.decode_dec (receive_minimum_l);
			result |= inactive_supply.decode_dec (inactive_supply_l);
			result |= password_fanout < 16;
			result |= password_fanout > 1024 * 1024;
			result |= io_threads == 0;
			result |= work_threads == 0;
		}
		catch (std::logic_error const &)
		{
			result = true;
		}
	}
	catch (std::runtime_error const &)
	{
		result = true;
	}
	return result;
}

raze::account raze::node_config::random_representative ()
{
	assert (preconfigured_representatives.size () > 0);
	size_t index (raze::random_pool.GenerateWord32 (0, preconfigured_representatives.size () - 1));
	auto result (preconfigured_representatives[index]);
	return result;
}

raze::vote_processor::vote_processor (raze::node & node_a) :
node (node_a)
{
}

raze::vote_result raze::vote_processor::vote (std::shared_ptr<raze::vote> vote_a, raze::endpoint endpoint_a)
{
	raze::vote_result result;
	{
		raze::transaction transaction (node.store.environment, nullptr, false);
		result = node.store.vote_validate (transaction, vote_a);
	}
	if (node.config.logging.vote_logging ())
	{
		char const * status;
		switch (result.code)
		{
			case raze::vote_code::invalid:
				status = "Invalid";
				break;
			case raze::vote_code::replay:
				status = "Replay";
				break;
			case raze::vote_code::vote:
			case raze::vote_code::vote2:
				status = "Vote";
				break;
		}
		BOOST_LOG (node.log) << boost::str (boost::format ("Vote from: %1% sequence: %2% block: %3% status: %4%") % vote_a->account.to_account () % std::to_string (vote_a->sequence) % vote_a->block->hash ().to_string () % status);
	}
	switch (result.code)
	{
		case raze::vote_code::vote:
		case raze::vote_code::vote2:
			node.observers.vote (vote_a, result.code, endpoint_a);
		case raze::vote_code::replay:
		case raze::vote_code::invalid:
			break;
	}
	return result;
}

void raze::rep_crawler::add (raze::block_hash const & hash_a)
{
	std::lock_guard<std::mutex> lock (mutex);
	active.insert (hash_a);
}

void raze::rep_crawler::remove (raze::block_hash const & hash_a)
{
	std::lock_guard<std::mutex> lock (mutex);
	active.erase (hash_a);
}

bool raze::rep_crawler::exists (raze::block_hash const & hash_a)
{
	std::lock_guard<std::mutex> lock (mutex);
	return active.count (hash_a) != 0;
}

raze::block_processor_item::block_processor_item (std::shared_ptr<raze::block> block_a) :
block_processor_item (block_a, false)
{
}

raze::block_processor_item::block_processor_item (std::shared_ptr<raze::block> block_a, bool force_a) :
block (block_a),
force (force_a)
{
}

raze::block_processor::block_processor (raze::node & node_a) :
stopped (false),
idle (true),
node (node_a)
{
}

raze::block_processor::~block_processor ()
{
	stop ();
}

void raze::block_processor::stop ()
{
	std::lock_guard<std::mutex> lock (mutex);
	stopped = true;
	condition.notify_all ();
}

void raze::block_processor::flush ()
{
	std::unique_lock<std::mutex> lock (mutex);
	while (!stopped && (!blocks.empty () || !idle))
	{
		condition.wait (lock);
	}
}

void raze::block_processor::add (raze::block_processor_item const & item_a)
{
	std::lock_guard<std::mutex> lock (mutex);
	blocks.push_back (item_a);
	condition.notify_all ();
}

void raze::block_processor::process_blocks ()
{
	std::unique_lock<std::mutex> lock (mutex);
	while (!stopped)
	{
		if (!blocks.empty ())
		{
			std::deque<raze::block_processor_item> blocks_processing;
			std::swap (blocks, blocks_processing);
			lock.unlock ();
			process_receive_many (blocks_processing);
			// Let other threads get an opportunity to transaction lock
			std::this_thread::yield ();
			lock.lock ();
		}
		else
		{
			idle = true;
			condition.notify_all ();
			condition.wait (lock);
			idle = false;
		}
	}
}

void raze::block_processor::process_receive_many (raze::block_processor_item const & item_a)
{
	std::deque<raze::block_processor_item> blocks_processing;
	blocks_processing.push_back (item_a);
	process_receive_many (blocks_processing);
}

void raze::block_processor::process_receive_many (std::deque<raze::block_processor_item> & blocks_processing)
{
	while (!blocks_processing.empty ())
	{
		std::deque<std::pair<std::shared_ptr<raze::block>, raze::process_return>> progress;
		{
			raze::transaction transaction (node.store.environment, nullptr, true);
			auto cutoff (std::chrono::steady_clock::now () + raze::transaction_timeout);
			while (!blocks_processing.empty () && std::chrono::steady_clock::now () < cutoff)
			{
				auto item (blocks_processing.front ());
				blocks_processing.pop_front ();
				auto hash2_new (node.store.hash2_calc (transaction, *item.block));
				if (item.force)
				{
					auto successor (node.ledger.successor (transaction, item.block->root ()));
					if (successor != nullptr)
					{
						auto hash2_existing (node.store.hash2_calc (transaction, *successor));
						if (hash2_new != hash2_existing)
						{
							// Replace our block with the winner and roll back any dependent blocks
							BOOST_LOG (node.log) << boost::str (boost::format ("Rolling back %1% and replacing with %2%") % hash2_existing.to_string () % hash2_new.to_string ());
							node.ledger.rollback (transaction, successor->hash ());
						}
					}
				}
				auto hash (item.block->hash ());
				auto process_result (process_receive_one (transaction, item.block));
				switch (process_result.code)
				{
					case raze::process_result::progress:
					{
						progress.push_back (std::make_pair (item.block, process_result));
					}
					case raze::process_result::old:
					{
						auto cached (node.store.unchecked_get (transaction, hash));
						for (auto i (cached.begin ()), n (cached.end ()); i != n; ++i)
						{
							node.store.unchecked_del (transaction, hash, **i);
							blocks_processing.push_front (raze::block_processor_item (*i));
						}
						std::lock_guard<std::mutex> lock (node.gap_cache.mutex);
						node.gap_cache.blocks.get<1> ().erase (hash);
						break;
					}
					default:
						break;
				}
			}
		}
		for (auto & i : progress)
		{
			node.observers.blocks (i.first, i.second.account, i.second.amount);
			if (i.second.amount > 0)
			{
				node.observers.account_balance (i.second.account, false);
				if (!i.second.pending_account.is_zero ())
				{
					node.observers.account_balance (i.second.pending_account, true);
				}
			}
		}
	}
}

raze::process_return raze::block_processor::process_receive_one (MDB_txn * transaction_a, std::shared_ptr<raze::block> block_a)
{
	raze::process_return result;
	result = node.ledger.process (transaction_a, *block_a);
	switch (result.code)
	{
		case raze::process_result::progress:
		{
			if (node.config.logging.ledger_logging ())
			{
				std::string block;
				block_a->serialize_json (block);
				BOOST_LOG (node.log) << boost::str (boost::format ("Processing block %1% %2%") % block_a->hash ().to_string () % block);
			}
			break;
		}
		case raze::process_result::gap_previous:
		{
			if (node.config.logging.ledger_logging ())
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("Gap previous for: %1%") % block_a->hash ().to_string ());
			}
			node.store.unchecked_put (transaction_a, block_a->previous (), block_a);
			node.gap_cache.add (transaction_a, block_a);
			break;
		}
		case raze::process_result::gap_source:
		{
			if (node.config.logging.ledger_logging ())
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("Gap source for: %1%") % block_a->hash ().to_string ());
			}
			node.store.unchecked_put (transaction_a, block_a->source (), block_a);
			node.gap_cache.add (transaction_a, block_a);
			break;
		}
		case raze::process_result::old:
		{
			{
				auto root (block_a->root ());
				auto hash (block_a->hash ());
				auto existing (node.store.block_get (transaction_a, hash));
				if (existing != nullptr)
				{
					// Replace block with one that has higher work value
					if (raze::work_value (root, block_a->block_work ()) > raze::work_value (root, existing->block_work ()))
					{
						auto account (node.ledger.account (transaction_a, hash));
						if (!raze::validate_message (account, hash, block_a->block_signature ()))
						{
							node.store.block_put (transaction_a, hash, *block_a, node.store.block_successor (transaction_a, hash));
							BOOST_LOG (node.log) << boost::str (boost::format ("Replacing block %1% with one that has higher work value") % hash.to_string ());
						}
					}
				}
				else
				{
					// Could have been rolled back, maybe
				}
			}
			if (node.config.logging.ledger_duplicate_logging ())
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("Old for: %1%") % block_a->hash ().to_string ());
			}
			break;
		}
		case raze::process_result::bad_signature:
		{
			if (node.config.logging.ledger_logging ())
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("Bad signature for: %1%") % block_a->hash ().to_string ());
			}
			break;
		}
		case raze::process_result::negative_spend:
		{
			if (node.config.logging.ledger_logging ())
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("Negative spend for: %1%") % block_a->hash ().to_string ());
			}
			break;
		}
		case raze::process_result::unreceivable:
		{
			if (node.config.logging.ledger_logging ())
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("Unreceivable for: %1%") % block_a->hash ().to_string ());
			}
			break;
		}
		case raze::process_result::not_receive_from_send:
		{
			if (node.config.logging.ledger_logging ())
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("Not receive from send for: %1%") % block_a->hash ().to_string ());
			}
			break;
		}
		case raze::process_result::fork:
		{
			if (!node.block_arrival.recent (block_a->hash ()))
			{
				// Only let the bootstrap attempt know about forked blocks that did not arrive via UDP.
				node.bootstrap_initiator.process_fork (transaction_a, block_a);
			}
			if (node.config.logging.ledger_logging ())
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("Fork for: %1% root: %2%") % block_a->hash ().to_string () % block_a->root ().to_string ());
			}
			break;
		}
		case raze::process_result::account_mismatch:
		{
			if (node.config.logging.ledger_logging ())
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("Account mismatch for: %1%") % block_a->hash ().to_string ());
			}
		}
		case raze::process_result::opened_burn_account:
		{
			BOOST_LOG (node.log) << boost::str (boost::format ("*** Rejecting open block for burn account ***: %1%") % block_a->hash ().to_string ());
		}
	}
	return result;
}

raze::node::node (raze::node_init & init_a, boost::asio::io_service & service_a, uint16_t peering_port_a, boost::filesystem::path const & application_path_a, raze::alarm & alarm_a, raze::logging const & logging_a, raze::work_pool & work_a) :
node (init_a, service_a, application_path_a, alarm_a, raze::node_config (peering_port_a, logging_a), work_a)
{
}

raze::node::node (raze::node_init & init_a, boost::asio::io_service & service_a, boost::filesystem::path const & application_path_a, raze::alarm & alarm_a, raze::node_config const & config_a, raze::work_pool & work_a) :
service (service_a),
config (config_a),
alarm (alarm_a),
work (work_a),
store (init_a.block_store_init, application_path_a / "data.ldb", config_a.lmdb_max_dbs),
gap_cache (*this),
ledger (store, config_a.inactive_supply.number ()),
active (*this),
wallets (init_a.block_store_init, *this),
network (*this, config.peering_port),
bootstrap_initiator (*this),
bootstrap (service_a, config.peering_port, *this),
peers (network.endpoint ()),
application_path (application_path_a),
port_mapping (*this),
vote_processor (*this),
warmed_up (0),
block_processor (*this),
block_processor_thread ([this]() { this->block_processor.process_blocks (); })
{
	wallets.observer = [this](bool active) {
		observers.wallet (active);
	};
	peers.peer_observer = [this](raze::endpoint const & endpoint_a) {
		observers.endpoint (endpoint_a);
	};
	peers.disconnect_observer = [this]() {
		observers.disconnect ();
	};
	observers.blocks.add ([this](std::shared_ptr<raze::block> block_a, raze::account const & account_a, raze::amount const & amount_a) {
		if (this->block_arrival.recent (block_a->hash ()))
		{
			raze::transaction transaction (store.environment, nullptr, true);
			active.start (transaction, block_a);
		}
	});
	observers.blocks.add ([this](std::shared_ptr<raze::block> block_a, raze::account const & account_a, raze::amount const & amount_a) {
		if (this->block_arrival.recent (block_a->hash ()))
		{
			auto node_l (shared_from_this ());
			background ([node_l, block_a, account_a, amount_a]() {
				if (!node_l->config.callback_address.empty ())
				{
					boost::property_tree::ptree event;
					event.add ("account", account_a.to_account ());
					event.add ("hash", block_a->hash ().to_string ());
					std::string block_text;
					block_a->serialize_json (block_text);
					event.add ("block", block_text);
					event.add ("amount", amount_a.to_string_dec ());
					std::stringstream ostream;
					boost::property_tree::write_json (ostream, event);
					ostream.flush ();
					auto body (std::make_shared<std::string> (ostream.str ()));
					auto address (node_l->config.callback_address);
					auto port (node_l->config.callback_port);
					auto target (std::make_shared<std::string> (node_l->config.callback_target));
					auto resolver (std::make_shared<boost::asio::ip::tcp::resolver> (node_l->service));
					resolver->async_resolve (boost::asio::ip::tcp::resolver::query (address, std::to_string (port)), [node_l, address, port, target, body, resolver](boost::system::error_code const & ec, boost::asio::ip::tcp::resolver::iterator i_a) {
						if (!ec)
						{
							for (auto i (i_a), n (boost::asio::ip::tcp::resolver::iterator{}); i != n; ++i)
							{
								auto sock (std::make_shared<boost::asio::ip::tcp::socket> (node_l->service));
								sock->async_connect (i->endpoint (), [node_l, target, body, sock, address, port](boost::system::error_code const & ec) {
									if (!ec)
									{
										auto req (std::make_shared<boost::beast::http::request<boost::beast::http::string_body>> ());
										req->method (boost::beast::http::verb::post);
										req->target (*target);
										req->version (11);
										req->insert (boost::beast::http::field::host, address);
										req->insert (boost::beast::http::field::content_type, "application/json");
										req->body () = *body;
										//req->prepare (*req);
										//boost::beast::http::prepare(req);
										req->prepare_payload ();
										boost::beast::http::async_write (*sock, *req, [node_l, sock, address, port, req](boost::system::error_code const & ec, size_t bytes_transferred) {
											if (!ec)
											{
												auto sb (std::make_shared<boost::beast::flat_buffer> ());
												auto resp (std::make_shared<boost::beast::http::response<boost::beast::http::string_body>> ());
												boost::beast::http::async_read (*sock, *sb, *resp, [node_l, sb, resp, sock, address, port](boost::system::error_code const & ec, size_t bytes_transferred) {
													if (!ec)
													{
														if (resp->result () == boost::beast::http::status::ok)
														{
														}
														else
														{
															if (node_l->config.logging.callback_logging ())
															{
																BOOST_LOG (node_l->log) << boost::str (boost::format ("Callback to %1%:%2% failed with status: %3%") % address % port % resp->result ());
															}
														}
													}
													else
													{
														if (node_l->config.logging.callback_logging ())
														{
															BOOST_LOG (node_l->log) << boost::str (boost::format ("Unable complete callback: %1%:%2% %3%") % address % port % ec.message ());
														}
													};
												});
											}
											else
											{
												if (node_l->config.logging.callback_logging ())
												{
													BOOST_LOG (node_l->log) << boost::str (boost::format ("Unable to send callback: %1%:%2% %3%") % address % port % ec.message ());
												}
											}
										});
									}
									else
									{
										if (node_l->config.logging.callback_logging ())
										{
											BOOST_LOG (node_l->log) << boost::str (boost::format ("Unable to connect to callback address: %1%:%2%, %3%") % address % port % ec.message ());
										}
									}
								});
							}
						}
						else
						{
							if (node_l->config.logging.callback_logging ())
							{
								BOOST_LOG (node_l->log) << boost::str (boost::format ("Error resolving callback: %1%:%2%, %3%") % address % port % ec.message ());
							}
						}
					});
				}
			});
		}
	});
	observers.endpoint.add ([this](raze::endpoint const & endpoint_a) {
		this->network.send_keepalive (endpoint_a);
		rep_query (*this, endpoint_a);
	});
	observers.vote.add ([this](std::shared_ptr<raze::vote> vote_a, raze::vote_code code, raze::endpoint const & endpoint_a) {
		assert (code == raze::vote_code::vote || code == raze::vote_code::vote2);
		if (!peers.hash2_aware (endpoint_a) || code == raze::vote_code::vote2)
		{
			active.vote (vote_a);
		}
	});
	observers.vote.add ([this](std::shared_ptr<raze::vote> vote_a, raze::vote_code, raze::endpoint const &) {
		this->gap_cache.vote (vote_a);
	});
	observers.vote.add ([this](std::shared_ptr<raze::vote> vote_a, raze::vote_code, raze::endpoint const & endpoint_a) {
		if (this->rep_crawler.exists (vote_a->block->hash ()))
		{
			auto weight_l (weight (vote_a->account));
			// We see a valid non-replay vote for a block we requested, this node is probably a representative
			if (peers.rep_response (endpoint_a, weight_l))
			{
				BOOST_LOG (log) << boost::str (boost::format ("Found a representative at %1%") % endpoint_a);
			}
		}
	});
	BOOST_LOG (log) << "Node starting, version: " << RAZE_VERSION_MAJOR << "." << RAZE_VERSION_MINOR;
	BOOST_LOG (log) << boost::str (boost::format ("Work pool running %1% threads") % work.threads.size ());
	if (!init_a.error ())
	{
		if (config.logging.node_lifetime_tracing ())
		{
			BOOST_LOG (log) << "Constructing node";
		}
		raze::transaction transaction (store.environment, nullptr, true);
		if (store.latest_begin (transaction) == store.latest_end ())
		{
			// Store was empty meaning we just created it, add the genesis block
			raze::genesis genesis;
			genesis.initialize (transaction, store);
		}
	}
	if (raze::raze_network == raze::raze_networks::raze_live_network)
	{
		extern const char raze_bootstrap_weights[];
		extern const size_t raze_bootstrap_weights_size;
		raze::bufferstream weight_stream ((const uint8_t *)raze_bootstrap_weights, raze_bootstrap_weights_size);
		raze::uint128_union block_height;
		if (!raze::read (weight_stream, block_height))
		{
			auto max_blocks = (uint64_t)block_height.number ();
			raze::transaction transaction (store.environment, nullptr, false);
			if (ledger.store.block_count (transaction).sum () < max_blocks)
			{
				ledger.bootstrap_weight_max_blocks = max_blocks;
				while (true)
				{
					raze::account account;
					if (raze::read (weight_stream, account.bytes))
					{
						break;
					}
					raze::amount weight;
					if (raze::read (weight_stream, weight.bytes))
					{
						break;
					}
					BOOST_LOG (log) << "Using bootstrap rep weight: " << account.to_account () << " -> " << weight.format_balance (Mraze_ratio, 0, true) << " XRB";
					ledger.bootstrap_weights[account] = weight.number ();
				}
			}
		}
	}
}

raze::node::~node ()
{
	if (config.logging.node_lifetime_tracing ())
	{
		BOOST_LOG (log) << "Destructing node";
	}
	stop ();
}

bool raze::node::copy_with_compaction (boost::filesystem::path const & destination_file)
{
	return !mdb_env_copy2 (store.environment.environment,
	destination_file.string ().c_str (), MDB_CP_COMPACT);
}

void raze::node::send_keepalive (raze::endpoint const & endpoint_a)
{
	auto endpoint_l (endpoint_a);
	if (endpoint_l.address ().is_v4 ())
	{
		endpoint_l = raze::endpoint (boost::asio::ip::address_v6::v4_mapped (endpoint_l.address ().to_v4 ()), endpoint_l.port ());
	}
	assert (endpoint_l.address ().is_v6 ());
	network.send_keepalive (endpoint_l);
}

raze::gap_cache::gap_cache (raze::node & node_a) :
node (node_a)
{
}

void raze::gap_cache::add (MDB_txn * transaction_a, std::shared_ptr<raze::block> block_a)
{
	auto hash (block_a->hash ());
	std::lock_guard<std::mutex> lock (mutex);
	auto existing (blocks.get<1> ().find (hash));
	if (existing != blocks.get<1> ().end ())
	{
		blocks.get<1> ().modify (existing, [](raze::gap_information & info) {
			info.arrival = std::chrono::steady_clock::now ();
		});
	}
	else
	{
		blocks.insert ({ std::chrono::steady_clock::now (), hash, std::unique_ptr<raze::votes> (new raze::votes (block_a)) });
		if (blocks.size () > max)
		{
			blocks.get<0> ().erase (blocks.get<0> ().begin ());
		}
	}
}

void raze::gap_cache::vote (std::shared_ptr<raze::vote> vote_a)
{
	raze::transaction transaction (node.store.environment, nullptr, false);
	std::lock_guard<std::mutex> lock (mutex);
	auto hash (vote_a->block->hash ());
	auto existing (blocks.get<1> ().find (hash));
	if (existing != blocks.get<1> ().end ())
	{
		existing->votes->vote (vote_a);
		auto winner (node.ledger.winner (transaction, *existing->votes));
		if (winner.first > bootstrap_threshold (transaction))
		{
			auto node_l (node.shared ());
			auto now (std::chrono::steady_clock::now ());
			node.alarm.add (raze::raze_network == raze::raze_networks::raze_test_network ? now + std::chrono::milliseconds (5) : now + std::chrono::seconds (5), [node_l, hash]() {
				raze::transaction transaction (node_l->store.environment, nullptr, false);
				if (!node_l->store.block_exists (transaction, hash))
				{
					if (!node_l->bootstrap_initiator.in_progress ())
					{
						BOOST_LOG (node_l->log) << boost::str (boost::format ("Missing confirmed block %1%") % hash.to_string ());
					}
					node_l->bootstrap_initiator.bootstrap ();
				}
			});
		}
	}
}

raze::uint128_t raze::gap_cache::bootstrap_threshold (MDB_txn * transaction_a)
{
	auto result ((node.ledger.supply (transaction_a) / 256) * node.config.bootstrap_fraction_numerator);
	return result;
}

void raze::gap_cache::purge_old ()
{
	auto cutoff (std::chrono::steady_clock::now () - std::chrono::seconds (10));
	std::lock_guard<std::mutex> lock (mutex);
	auto done (false);
	while (!done && !blocks.empty ())
	{
		auto first (blocks.get<1> ().begin ());
		if (first->arrival < cutoff)
		{
			blocks.get<1> ().erase (first);
		}
		else
		{
			done = true;
		}
	}
}

void raze::network::confirm_send (raze::confirm_ack const & confirm_a, std::shared_ptr<std::vector<uint8_t>> bytes_a, raze::endpoint const & endpoint_a)
{
	if (node.config.logging.network_publish_logging ())
	{
		BOOST_LOG (node.log) << boost::str (boost::format ("Sending confirm_ack for block %1% to %2% sequence %3%") % confirm_a.vote->block->hash ().to_string () % endpoint_a % std::to_string (confirm_a.vote->sequence));
	}
	std::weak_ptr<raze::node> node_w (node.shared ());
	++outgoing.confirm_ack;
	node.network.send_buffer (bytes_a->data (), bytes_a->size (), endpoint_a, [bytes_a, node_w, endpoint_a](boost::system::error_code const & ec, size_t size_a) {
		if (auto node_l = node_w.lock ())
		{
			if (ec && node_l->config.logging.network_logging ())
			{
				BOOST_LOG (node_l->log) << boost::str (boost::format ("Error broadcasting confirm_ack to %1%: %2%") % endpoint_a % ec.message ());
			}
		}
	});
}

void raze::node::process_active (std::shared_ptr<raze::block> incoming)
{
	block_arrival.add (incoming->hash ());
	block_processor.add (incoming);
}

raze::process_return raze::node::process (raze::block const & block_a)
{
	raze::transaction transaction (store.environment, nullptr, true);
	auto result (ledger.process (transaction, block_a));
	return result;
}

// Simulating with sqrt_broadcast_simulate shows we only need to broadcast to sqrt(total_peers) random peers in order to successfully publish to everyone with high probability
std::vector<raze::endpoint> raze::peer_container::list_sqrt ()
{
	auto peers (random_set (2 * size_sqrt ()));
	std::vector<raze::endpoint> result;
	result.reserve (peers.size ());
	for (auto i (peers.begin ()), n (peers.end ()); i != n; ++i)
	{
		result.push_back (*i);
	}
	return result;
}

std::vector<raze::endpoint> raze::peer_container::list ()
{
	std::vector<raze::endpoint> result;
	std::lock_guard<std::mutex> lock (mutex);
	result.reserve (peers.size ());
	for (auto i (peers.begin ()), j (peers.end ()); i != j; ++i)
	{
		result.push_back (i->endpoint);
	}
	std::random_shuffle (result.begin (), result.end ());
	return result;
}

std::map<raze::endpoint, unsigned> raze::peer_container::list_version ()
{
	std::map<raze::endpoint, unsigned> result;
	std::lock_guard<std::mutex> lock (mutex);
	for (auto i (peers.begin ()), j (peers.end ()); i != j; ++i)
	{
		result.insert (std::pair<raze::endpoint, unsigned> (i->endpoint, i->network_version));
	}
	return result;
}

raze::endpoint raze::peer_container::bootstrap_peer ()
{
	raze::endpoint result (boost::asio::ip::address_v6::any (), 0);
	std::lock_guard<std::mutex> lock (mutex);
	;
	for (auto i (peers.get<4> ().begin ()), n (peers.get<4> ().end ()); i != n;)
	{
		if (i->network_version >= 0x5)
		{
			result = i->endpoint;
			peers.get<4> ().modify (i, [](raze::peer_information & peer_a) {
				peer_a.last_bootstrap_attempt = std::chrono::steady_clock::now ();
			});
			i = n;
		}
		else
		{
			++i;
		}
	}
	return result;
}

bool raze::parse_port (std::string const & string_a, uint16_t & port_a)
{
	bool result;
	size_t converted;
	port_a = std::stoul (string_a, &converted);
	result = converted != string_a.size () || converted > std::numeric_limits<uint16_t>::max ();
	return result;
}

bool raze::parse_address_port (std::string const & string, boost::asio::ip::address & address_a, uint16_t & port_a)
{
	auto result (false);
	auto port_position (string.rfind (':'));
	if (port_position != std::string::npos && port_position > 0)
	{
		std::string port_string (string.substr (port_position + 1));
		try
		{
			uint16_t port;
			result = parse_port (port_string, port);
			if (!result)
			{
				boost::system::error_code ec;
				auto address (boost::asio::ip::address_v6::from_string (string.substr (0, port_position), ec));
				if (ec == 0)
				{
					address_a = address;
					port_a = port;
				}
				else
				{
					result = true;
				}
			}
			else
			{
				result = true;
			}
		}
		catch (...)
		{
			result = true;
		}
	}
	else
	{
		result = true;
	}
	return result;
}

bool raze::parse_endpoint (std::string const & string, raze::endpoint & endpoint_a)
{
	boost::asio::ip::address address;
	uint16_t port;
	auto result (parse_address_port (string, address, port));
	if (!result)
	{
		endpoint_a = raze::endpoint (address, port);
	}
	return result;
}

bool raze::parse_tcp_endpoint (std::string const & string, raze::tcp_endpoint & endpoint_a)
{
	boost::asio::ip::address address;
	uint16_t port;
	auto result (parse_address_port (string, address, port));
	if (!result)
	{
		endpoint_a = raze::tcp_endpoint (address, port);
	}
	return result;
}

void raze::node::start ()
{
	if (store_version () < 11)
	{
		store_update ();
	}
	network.receive ();
	ongoing_keepalive ();
	ongoing_bootstrap ();
	ongoing_store_flush ();
	ongoing_rep_crawl ();
	bootstrap.start ();
	backup_wallet ();
	active.announce_votes ();
	port_mapping.start ();
	add_initial_peers ();
	observers.started ();
}

void raze::node::store_update ()
{
	if (store_version () < 11)
	{
		BOOST_LOG (log) << "Updating block store";
		store.upgrade_v10_to_v11 ();
	}
}

void raze::node::stop ()
{
	BOOST_LOG (log) << "Node stopping";
	block_processor.stop ();
	if (block_processor_thread.joinable ())
	{
		block_processor_thread.join ();
	}
	active.stop ();
	network.stop ();
	bootstrap_initiator.stop ();
	bootstrap.stop ();
	port_mapping.stop ();
	wallets.stop ();
	if (block_processor_thread.joinable ())
	{
		block_processor_thread.join ();
	}
}

void raze::node::keepalive_preconfigured (std::vector<std::string> const & peers_a)
{
	for (auto i (peers_a.begin ()), n (peers_a.end ()); i != n; ++i)
	{
		keepalive (*i, raze::network::node_port);
	}
}

raze::block_hash raze::node::latest (raze::account const & account_a)
{
	raze::transaction transaction (store.environment, nullptr, false);
	return ledger.latest (transaction, account_a);
}

raze::uint128_t raze::node::balance (raze::account const & account_a)
{
	raze::transaction transaction (store.environment, nullptr, false);
	return ledger.account_balance (transaction, account_a);
}

std::unique_ptr<raze::block> raze::node::block (raze::block_hash const & hash_a)
{
	raze::transaction transaction (store.environment, nullptr, false);
	return store.block_get (transaction, hash_a);
}

std::pair<raze::uint128_t, raze::uint128_t> raze::node::balance_pending (raze::account const & account_a)
{
	std::pair<raze::uint128_t, raze::uint128_t> result;
	raze::transaction transaction (store.environment, nullptr, false);
	result.first = ledger.account_balance (transaction, account_a);
	result.second = ledger.account_pending (transaction, account_a);
	return result;
}

raze::uint128_t raze::node::weight (raze::account const & account_a)
{
	raze::transaction transaction (store.environment, nullptr, false);
	return ledger.weight (transaction, account_a);
}

raze::account raze::node::representative (raze::account const & account_a)
{
	raze::transaction transaction (store.environment, nullptr, false);
	raze::account_info info;
	raze::account result (0);
	if (!store.account_get (transaction, account_a, info))
	{
		result = info.rep_block;
	}
	return result;
}

void raze::node::ongoing_keepalive ()
{
	keepalive_preconfigured (config.preconfigured_peers);
	auto peers_l (peers.purge_list (std::chrono::steady_clock::now () - cutoff));
	for (auto i (peers_l.begin ()), j (peers_l.end ()); i != j && std::chrono::steady_clock::now () - i->last_attempt > period; ++i)
	{
		network.send_keepalive (i->endpoint);
	}
	std::weak_ptr<raze::node> node_w (shared_from_this ());
	alarm.add (std::chrono::steady_clock::now () + period, [node_w]() {
		if (auto node_l = node_w.lock ())
		{
			node_l->ongoing_keepalive ();
		}
	});
}

void raze::node::ongoing_rep_crawl ()
{
	auto now (std::chrono::steady_clock::now ());
	auto peers_l (peers.rep_crawl ());
	rep_query (*this, peers_l);
	if (network.on)
	{
		std::weak_ptr<raze::node> node_w (shared_from_this ());
		alarm.add (now + period, [node_w]() {
			if (auto node_l = node_w.lock ())
			{
				node_l->ongoing_rep_crawl ();
			}
		});
	}
}

void raze::node::ongoing_bootstrap ()
{
	auto next_wakeup (300);
	if (warmed_up < 3)
	{
		// Re-attempt bootstrapping more aggressively on startup
		next_wakeup = 5;
		if (!bootstrap_initiator.in_progress () && !peers.empty ())
		{
			++warmed_up;
		}
	}
	bootstrap_initiator.bootstrap ();
	std::weak_ptr<raze::node> node_w (shared_from_this ());
	alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (next_wakeup), [node_w]() {
		if (auto node_l = node_w.lock ())
		{
			node_l->ongoing_bootstrap ();
		}
	});
}

void raze::node::ongoing_store_flush ()
{
	{
		raze::transaction transaction (store.environment, nullptr, true);
		store.flush (transaction);
	}
	std::weak_ptr<raze::node> node_w (shared_from_this ());
	alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (5), [node_w]() {
		if (auto node_l = node_w.lock ())
		{
			node_l->ongoing_store_flush ();
		}
	});
}

void raze::node::backup_wallet ()
{
	raze::transaction transaction (store.environment, nullptr, false);
	for (auto i (wallets.items.begin ()), n (wallets.items.end ()); i != n; ++i)
	{
		auto backup_path (application_path / "backup");
		boost::filesystem::create_directories (backup_path);
		i->second->store.write_backup (transaction, backup_path / (i->first.to_string () + ".json"));
	}
	auto this_l (shared ());
	alarm.add (std::chrono::steady_clock::now () + backup_interval, [this_l]() {
		this_l->backup_wallet ();
	});
}

int raze::node::price (raze::uint128_t const & balance_a, int amount_a)
{
	assert (balance_a >= amount_a * raze::Graze_ratio);
	auto balance_l (balance_a);
	double result (0.0);
	for (auto i (0); i < amount_a; ++i)
	{
		balance_l -= raze::Graze_ratio;
		auto balance_scaled ((balance_l / raze::Mraze_ratio).convert_to<double> ());
		auto units (balance_scaled / 1000.0);
		auto unit_price (((free_cutoff - units) / free_cutoff) * price_max);
		result += std::min (std::max (0.0, unit_price), price_max);
	}
	return static_cast<int> (result * 100.0);
}

namespace
{
class work_request
{
public:
	work_request (boost::asio::io_service & service_a, boost::asio::ip::address address_a, uint16_t port_a) :
	address (address_a),
	port (port_a),
	socket (service_a)
	{
	}
	boost::asio::ip::address address;
	uint16_t port;
	boost::beast::flat_buffer buffer;
	boost::beast::http::response<boost::beast::http::string_body> response;
	boost::asio::ip::tcp::socket socket;
};
class distributed_work : public std::enable_shared_from_this<distributed_work>
{
public:
	distributed_work (std::shared_ptr<raze::node> const & node_a, raze::block_hash const & root_a, std::function<void(uint64_t)> callback_a) :
	callback (callback_a),
	node (node_a),
	root (root_a)
	{
		completed.clear ();
		for (auto & i : node_a->config.work_peers)
		{
			outstanding[i.first] = i.second;
		}
	}
	void start ()
	{
		if (!outstanding.empty ())
		{
			auto this_l (shared_from_this ());
			std::lock_guard<std::mutex> lock (mutex);
			for (auto const & i : outstanding)
			{
				auto host (i.first);
				auto service (i.second);
				node->background ([this_l, host, service]() {
					auto connection (std::make_shared<work_request> (this_l->node->service, host, service));
					connection->socket.async_connect (raze::tcp_endpoint (host, service), [this_l, connection](boost::system::error_code const & ec) {
						if (!ec)
						{
							std::string request_string;
							{
								boost::property_tree::ptree request;
								request.put ("action", "work_generate");
								request.put ("hash", this_l->root.to_string ());
								std::stringstream ostream;
								boost::property_tree::write_json (ostream, request);
								request_string = ostream.str ();
							}
							auto request (std::make_shared<boost::beast::http::request<boost::beast::http::string_body>> ());
							request->method (boost::beast::http::verb::post);
							request->target ("/");
							request->version (11);
							request->body () = request_string;
							request->prepare_payload ();
							boost::beast::http::async_write (connection->socket, *request, [this_l, connection, request](boost::system::error_code const & ec, size_t bytes_transferred) {
								if (!ec)
								{
									boost::beast::http::async_read (connection->socket, connection->buffer, connection->response, [this_l, connection](boost::system::error_code const & ec, size_t bytes_transferred) {
										if (!ec)
										{
											if (connection->response.result () == boost::beast::http::status::ok)
											{
												this_l->success (connection->response.body (), connection->address);
											}
											else
											{
												BOOST_LOG (this_l->node->log) << boost::str (boost::format ("Work peer %1% responded with an error %2%") % connection->address % connection->port);
												this_l->failure (connection->address);
											}
										}
										else
										{
											BOOST_LOG (this_l->node->log) << boost::str (boost::format ("Unable to read from work_peer %1% %2%") % connection->address % connection->port);
											this_l->failure (connection->address);
										}
									});
								}
								else
								{
									BOOST_LOG (this_l->node->log) << boost::str (boost::format ("Unable to write to work_peer %1% %2%") % connection->address % connection->port);
									this_l->failure (connection->address);
								}
							});
						}
						else
						{
							BOOST_LOG (this_l->node->log) << boost::str (boost::format ("Unable to connect to work_peer %1% %2%") % connection->address % connection->port);
							this_l->failure (connection->address);
						}
					});
				});
			}
		}
		else
		{
			handle_failure (true);
		}
	}
	void stop ()
	{
		auto this_l (shared_from_this ());
		std::lock_guard<std::mutex> lock (mutex);
		for (auto const & i : outstanding)
		{
			auto host (i.first);
			auto service (i.second);
			node->background ([this_l, host, service]() {
				std::string request_string;
				{
					boost::property_tree::ptree request;
					request.put ("action", "work_cancel");
					request.put ("hash", this_l->root.to_string ());
					std::stringstream ostream;
					boost::property_tree::write_json (ostream, request);
					request_string = ostream.str ();
				}
				boost::beast::http::request<boost::beast::http::string_body> request;
				request.method (boost::beast::http::verb::post);
				request.target ("/");
				request.version (11);
				request.body () = request_string;
				request.prepare_payload ();
				auto socket (std::make_shared<boost::asio::ip::tcp::socket> (this_l->node->service));
				boost::beast::http::async_write (*socket, request, [socket](boost::system::error_code const & ec, size_t bytes_transferred) {
				});
			});
		}
		outstanding.clear ();
	}
	void success (std::string const & body_a, boost::asio::ip::address const & address)
	{
		auto last (remove (address));
		std::stringstream istream (body_a);
		try
		{
			boost::property_tree::ptree result;
			boost::property_tree::read_json (istream, result);
			auto work_text (result.get<std::string> ("work"));
			uint64_t work;
			if (!raze::from_string_hex (work_text, work))
			{
				if (!raze::work_validate (root, work))
				{
					set_once (work);
					stop ();
				}
				else
				{
					BOOST_LOG (node->log) << boost::str (boost::format ("Incorrect work response from %1% for root %2% value %3%") % address % root.to_string () % work_text);
					handle_failure (last);
				}
			}
			else
			{
				BOOST_LOG (node->log) << boost::str (boost::format ("Work response from %1% wasn't a number %2%") % address % work_text);
				handle_failure (last);
			}
		}
		catch (...)
		{
			BOOST_LOG (node->log) << boost::str (boost::format ("Work response from %1% wasn't parsable %2%") % address % body_a);
			handle_failure (last);
		}
	}
	void set_once (uint64_t work_a)
	{
		if (!completed.test_and_set ())
		{
			callback (work_a);
		}
	}
	void failure (boost::asio::ip::address const & address)
	{
		auto last (remove (address));
		handle_failure (last);
	}
	void handle_failure (bool last)
	{
		if (last)
		{
			if (!completed.test_and_set ())
			{
				auto callback_l (callback);
				node->work.generate (root, [callback_l](boost::optional<uint64_t> const & work_a) {
					callback_l (work_a.value ());
				});
			}
		}
	}
	bool remove (boost::asio::ip::address const & address)
	{
		std::lock_guard<std::mutex> lock (mutex);
		outstanding.erase (address);
		return outstanding.empty ();
	}
	std::function<void(uint64_t)> callback;
	std::shared_ptr<raze::node> node;
	raze::block_hash root;
	std::mutex mutex;
	std::map<boost::asio::ip::address, uint16_t> outstanding;
	std::atomic_flag completed;
};
}

void raze::node::generate_work (raze::block & block_a)
{
	block_a.block_work_set (generate_work (block_a.root ()));
}

void raze::node::generate_work (raze::uint256_union const & hash_a, std::function<void(uint64_t)> callback_a)
{
	auto work_generation (std::make_shared<distributed_work> (shared (), hash_a, callback_a));
	work_generation->start ();
}

uint64_t raze::node::generate_work (raze::uint256_union const & hash_a)
{
	std::promise<uint64_t> promise;
	generate_work (hash_a, [&promise](uint64_t work_a) {
		promise.set_value (work_a);
	});
	return promise.get_future ().get ();
}

void raze::node::add_initial_peers ()
{
}

namespace
{
class confirmed_visitor : public raze::block_visitor
{
public:
	confirmed_visitor (raze::node & node_a, std::shared_ptr<raze::block> block_a) :
	node (node_a),
	block (block_a)
	{
	}
	virtual ~confirmed_visitor () = default;
	void send_block (raze::send_block const & block_a) override
	{
		for (auto i (node.wallets.items.begin ()), n (node.wallets.items.end ()); i != n; ++i)
		{
			auto wallet (i->second);
			if (wallet->exists (block_a.hashables.destination))
			{
				raze::account representative;
				raze::pending_info pending;
				raze::transaction transaction (node.store.environment, nullptr, false);
				representative = wallet->store.representative (transaction);
				auto error (node.store.pending_get (transaction, raze::pending_key (block_a.hashables.destination, block_a.hash ()), pending));
				if (!error)
				{
					auto node_l (node.shared ());
					auto amount (pending.amount.number ());
					assert (block.get () == &block_a);
					wallet->receive_async (block, representative, amount, [](std::shared_ptr<raze::block>) {});
				}
				else
				{
					if (node.config.logging.ledger_duplicate_logging ())
					{
						BOOST_LOG (node.log) << boost::str (boost::format ("Block confirmed before timeout %1%") % block_a.hash ().to_string ());
					}
				}
			}
		}
	}
	void receive_block (raze::receive_block const &) override
	{
	}
	void open_block (raze::open_block const &) override
	{
	}
	void change_block (raze::change_block const &) override
	{
	}
	raze::node & node;
	std::shared_ptr<raze::block> block;
};
}

void raze::node::process_confirmed (std::shared_ptr<raze::block> confirmed_a)
{
	confirmed_visitor visitor (*this, confirmed_a);
	confirmed_a->visit (visitor);
}

void raze::node::process_message (raze::message & message_a, raze::endpoint const & sender_a)
{
	network_message_visitor visitor (*this, sender_a);
	message_a.visit (visitor);
}

raze::endpoint raze::network::endpoint ()
{
	boost::system::error_code ec;
	auto port (socket.local_endpoint (ec).port ());
	if (ec)
	{
		BOOST_LOG (node.log) << "Unable to retrieve port: " << ec.message ();
	}
	return raze::endpoint (boost::asio::ip::address_v6::loopback (), port);
}

void raze::block_arrival::add (raze::block_hash const & hash_a)
{
	std::lock_guard<std::mutex> lock (mutex);
	auto now (std::chrono::steady_clock::now ());
	arrival.insert (raze::block_arrival_info{ now, hash_a });
}

bool raze::block_arrival::recent (raze::block_hash const & hash_a)
{
	std::lock_guard<std::mutex> lock (mutex);
	auto now (std::chrono::steady_clock::now ());
	while (!arrival.empty () && arrival.begin ()->arrival + std::chrono::seconds (60) < now)
	{
		arrival.erase (arrival.begin ());
	}
	return arrival.get<1> ().find (hash_a) != arrival.get<1> ().end ();
}

std::unordered_set<raze::endpoint> raze::peer_container::random_set (size_t count_a)
{
	std::unordered_set<raze::endpoint> result;
	result.reserve (count_a);
	std::lock_guard<std::mutex> lock (mutex);
	// Stop trying to fill result with random samples after this many attempts
	auto random_cutoff (count_a * 2);
	auto peers_size (peers.size ());
	// Usually count_a will be much smaller than peers.size()
	// Otherwise make sure we have a cutoff on attempting to randomly fill
	if (!peers.empty ())
	{
		for (auto i (0); i < random_cutoff && result.size () < count_a; ++i)
		{
			auto index (random_pool.GenerateWord32 (0, peers_size - 1));
			result.insert (peers.get<3> ()[index].endpoint);
		}
	}
	// Fill the remainder with most recent contact
	for (auto i (peers.get<1> ().begin ()), n (peers.get<1> ().end ()); i != n && result.size () < count_a; ++i)
	{
		result.insert (i->endpoint);
	}
	return result;
}

void raze::peer_container::random_fill (std::array<raze::endpoint, 8> & target_a)
{
	auto peers (random_set (target_a.size ()));
	assert (peers.size () <= target_a.size ());
	auto endpoint (raze::endpoint (boost::asio::ip::address_v6{}, 0));
	assert (endpoint.address ().is_v6 ());
	std::fill (target_a.begin (), target_a.end (), endpoint);
	auto j (target_a.begin ());
	for (auto i (peers.begin ()), n (peers.end ()); i != n; ++i, ++j)
	{
		assert (i->address ().is_v6 ());
		assert (j < target_a.end ());
		*j = *i;
	}
}

// Request a list of the top known representatives
std::vector<raze::peer_information> raze::peer_container::representatives (size_t count_a)
{
	std::vector<peer_information> result;
	result.reserve (std::min (count_a, size_t (16)));
	std::lock_guard<std::mutex> lock (mutex);
	for (auto i (peers.get<6> ().begin ()), n (peers.get<6> ().end ()); i != n && result.size () < count_a; ++i)
	{
		if (!i->rep_weight.is_zero ())
		{
			result.push_back (*i);
		}
	}
	return result;
}

std::vector<raze::peer_information> raze::peer_container::purge_list (std::chrono::steady_clock::time_point const & cutoff)
{
	std::vector<raze::peer_information> result;
	{
		std::lock_guard<std::mutex> lock (mutex);
		auto pivot (peers.get<1> ().lower_bound (cutoff));
		result.assign (pivot, peers.get<1> ().end ());
		// Remove peers that haven't been heard from past the cutoff
		peers.get<1> ().erase (peers.get<1> ().begin (), pivot);
		for (auto i (peers.begin ()), n (peers.end ()); i != n; ++i)
		{
			peers.modify (i, [](raze::peer_information & info) { info.last_attempt = std::chrono::steady_clock::now (); });
		}

		// Remove keepalive attempt tracking for attempts older than cutoff
		auto attempts_pivot (attempts.get<1> ().lower_bound (cutoff));
		attempts.get<1> ().erase (attempts.get<1> ().begin (), attempts_pivot);
	}
	if (result.empty ())
	{
		disconnect_observer ();
	}
	return result;
}

std::vector<raze::endpoint> raze::peer_container::rep_crawl ()
{
	std::vector<raze::endpoint> result;
	result.reserve (8);
	std::lock_guard<std::mutex> lock (mutex);
	auto count (0);
	for (auto i (peers.get<5> ().begin ()), n (peers.get<5> ().end ()); i != n && count < 8; ++i, ++count)
	{
		result.push_back (i->endpoint);
	};
	return result;
}

size_t raze::peer_container::size ()
{
	std::lock_guard<std::mutex> lock (mutex);
	return peers.size ();
}

size_t raze::peer_container::size_sqrt ()
{
	auto result (std::ceil (std::sqrt (size ())));
	return result;
}

bool raze::peer_container::empty ()
{
	return size () == 0;
}

bool raze::peer_container::hash2_aware (raze::endpoint const & endpoint_a)
{
	std::lock_guard<std::mutex> lock (mutex);
	auto result (false);
	auto existing (peers.find (endpoint_a));
	if (existing != peers.end ())
	{
		result = existing->network_version >= 0x6;
	}
	return result;
}

bool raze::peer_container::not_a_peer (raze::endpoint const & endpoint_a)
{
	bool result (false);
	if (endpoint_a.address ().to_v6 ().is_unspecified ())
	{
		result = true;
	}
	else if (raze::reserved_address (endpoint_a))
	{
		result = true;
	}
	else if (endpoint_a == self)
	{
		result = true;
	}
	return result;
}

bool raze::peer_container::rep_response (raze::endpoint const & endpoint_a, raze::amount const & weight_a)
{
	auto updated (false);
	std::lock_guard<std::mutex> lock (mutex);
	auto existing (peers.find (endpoint_a));
	if (existing != peers.end ())
	{
		peers.modify (existing, [weight_a, &updated](raze::peer_information & info) {
			info.last_rep_response = std::chrono::steady_clock::now ();
			if (info.rep_weight < weight_a)
			{
				updated = true;
				info.rep_weight = weight_a;
			}
		});
	}
	return updated;
}

void raze::peer_container::rep_request (raze::endpoint const & endpoint_a)
{
	std::lock_guard<std::mutex> lock (mutex);
	auto existing (peers.find (endpoint_a));
	if (existing != peers.end ())
	{
		peers.modify (existing, [](raze::peer_information & info) {
			info.last_rep_request = std::chrono::steady_clock::now ();
		});
	}
}

bool raze::peer_container::reachout (raze::endpoint const & endpoint_a)
{
	auto result (false);
	// Don't contact invalid IPs
	result |= not_a_peer (endpoint_a);
	// Don't keepalive to nodes that already sent us something
	result |= known_peer (endpoint_a);
	std::lock_guard<std::mutex> lock (mutex);
	auto existing (attempts.find (endpoint_a));
	result |= existing != attempts.end ();
	attempts.insert ({ endpoint_a, std::chrono::steady_clock::now () });
	return result;
}

bool raze::peer_container::insert (raze::endpoint const & endpoint_a, unsigned version_a)
{
	auto unknown (false);
	auto result (not_a_peer (endpoint_a));
	if (!result)
	{
		std::lock_guard<std::mutex> lock (mutex);
		auto existing (peers.find (endpoint_a));
		if (existing != peers.end ())
		{
			peers.modify (existing, [](raze::peer_information & info) {
				info.last_contact = std::chrono::steady_clock::now ();
			});
			result = true;
		}
		else
		{
			peers.insert (raze::peer_information (endpoint_a, version_a));
			unknown = true;
		}
	}
	if (unknown && !result)
	{
		peer_observer (endpoint_a);
	}
	return result;
}

namespace
{
boost::asio::ip::address_v6 mapped_from_v4_bytes (unsigned long address_a)
{
	return boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4 (address_a));
}
}

bool raze::reserved_address (raze::endpoint const & endpoint_a)
{
	assert (endpoint_a.address ().is_v6 ());
	auto bytes (endpoint_a.address ().to_v6 ());
	auto result (false);
	static auto const rfc1700_min (mapped_from_v4_bytes (0x00000000ul));
	static auto const rfc1700_max (mapped_from_v4_bytes (0x00fffffful));
	static auto const ipv4_loopback_min (mapped_from_v4_bytes (0x7f000000ul));
	static auto const ipv4_loopback_max (mapped_from_v4_bytes (0x7ffffffful));
	static auto const rfc5737_1_min (mapped_from_v4_bytes (0xc0000200ul));
	static auto const rfc5737_1_max (mapped_from_v4_bytes (0xc00002fful));
	static auto const rfc5737_2_min (mapped_from_v4_bytes (0xc6336400ul));
	static auto const rfc5737_2_max (mapped_from_v4_bytes (0xc63364fful));
	static auto const rfc5737_3_min (mapped_from_v4_bytes (0xcb007100ul));
	static auto const rfc5737_3_max (mapped_from_v4_bytes (0xcb0071fful));
	static auto const ipv4_multicast_min (mapped_from_v4_bytes (0xe0000000ul));
	static auto const ipv4_multicast_max (mapped_from_v4_bytes (0xeffffffful));
	static auto const rfc6890_min (mapped_from_v4_bytes (0xf0000000ul));
	static auto const rfc6890_max (mapped_from_v4_bytes (0xfffffffful));
	static auto const rfc6666_min (boost::asio::ip::address_v6::from_string ("100::"));
	static auto const rfc6666_max (boost::asio::ip::address_v6::from_string ("100::ffff:ffff:ffff:ffff"));
	static auto const rfc3849_min (boost::asio::ip::address_v6::from_string ("2001:db8::"));
	static auto const rfc3849_max (boost::asio::ip::address_v6::from_string ("2001:db8:ffff:ffff:ffff:ffff:ffff:ffff"));
	static auto const ipv6_multicast_min (boost::asio::ip::address_v6::from_string ("ff00::"));
	static auto const ipv6_multicast_max (boost::asio::ip::address_v6::from_string ("ff00:ffff:ffff:ffff:ffff:ffff:ffff:ffff"));
	if (bytes >= rfc1700_min && bytes <= rfc1700_max)
	{
		result = true;
	}
	else if (bytes >= rfc5737_1_min && bytes <= rfc5737_1_max)
	{
		result = true;
	}
	else if (bytes >= rfc5737_2_min && bytes <= rfc5737_2_max)
	{
		result = true;
	}
	else if (bytes >= rfc5737_3_min && bytes <= rfc5737_3_max)
	{
		result = true;
	}
	else if (bytes >= ipv4_multicast_min && bytes <= ipv4_multicast_max)
	{
		result = true;
	}
	else if (bytes >= rfc6890_min && bytes <= rfc6890_max)
	{
		result = true;
	}
	else if (bytes >= rfc6666_min && bytes <= rfc6666_max)
	{
		result = true;
	}
	else if (bytes >= rfc3849_min && bytes <= rfc3849_max)
	{
		result = true;
	}
	else if (bytes >= ipv6_multicast_min && bytes <= ipv6_multicast_max)
	{
		result = true;
	}
	else if (bytes.is_loopback () && raze::raze_network != raze::raze_networks::raze_test_network)
	{
		result = true;
	}
	else if (bytes >= ipv4_loopback_min && bytes <= ipv4_loopback_max && raze::raze_network != raze::raze_networks::raze_test_network)
	{
		result = true;
	}
	return result;
}

raze::peer_information::peer_information (raze::endpoint const & endpoint_a, unsigned network_version_a) :
endpoint (endpoint_a),
last_contact (std::chrono::steady_clock::now ()),
last_attempt (last_contact),
last_bootstrap_attempt (std::chrono::steady_clock::time_point ()),
last_rep_request (std::chrono::steady_clock::time_point ()),
last_rep_response (std::chrono::steady_clock::time_point ()),
rep_weight (0),
network_version (network_version_a)
{
}

raze::peer_information::peer_information (raze::endpoint const & endpoint_a, std::chrono::steady_clock::time_point const & last_contact_a, std::chrono::steady_clock::time_point const & last_attempt_a) :
endpoint (endpoint_a),
last_contact (last_contact_a),
last_attempt (last_attempt_a),
last_bootstrap_attempt (std::chrono::steady_clock::time_point ()),
last_rep_request (std::chrono::steady_clock::time_point ()),
last_rep_response (std::chrono::steady_clock::time_point ()),
rep_weight (0)
{
}

raze::peer_container::peer_container (raze::endpoint const & self_a) :
self (self_a),
peer_observer ([](raze::endpoint const &) {}),
disconnect_observer ([]() {})
{
}

void raze::peer_container::contacted (raze::endpoint const & endpoint_a, unsigned version_a)
{
	auto endpoint_l (endpoint_a);
	if (endpoint_l.address ().is_v4 ())
	{
		endpoint_l = raze::endpoint (boost::asio::ip::address_v6::v4_mapped (endpoint_l.address ().to_v4 ()), endpoint_l.port ());
	}
	assert (endpoint_l.address ().is_v6 ());
	insert (endpoint_l, version_a);
}

void raze::network::send_buffer (uint8_t const * data_a, size_t size_a, raze::endpoint const & endpoint_a, std::function<void(boost::system::error_code const &, size_t)> callback_a)
{
	std::unique_lock<std::mutex> lock (socket_mutex);
	if (node.config.logging.network_packet_logging ())
	{
		BOOST_LOG (node.log) << "Sending packet";
	}
	socket.async_send_to (boost::asio::buffer (data_a, size_a), endpoint_a, [this, callback_a](boost::system::error_code const & ec, size_t size_a) {
		callback_a (ec, size_a);
		if (this->node.config.logging.network_packet_logging ())
		{
			BOOST_LOG (this->node.log) << "Packet send complete";
		}
	});
}

bool raze::peer_container::known_peer (raze::endpoint const & endpoint_a)
{
	std::lock_guard<std::mutex> lock (mutex);
	auto existing (peers.find (endpoint_a));
	return existing != peers.end ();
}

std::shared_ptr<raze::node> raze::node::shared ()
{
	return shared_from_this ();
}

raze::election::election (MDB_txn * transaction_a, raze::node & node_a, std::shared_ptr<raze::block> block_a, std::function<void(std::shared_ptr<raze::block>, bool)> const & confirmation_action_a) :
confirmation_action (confirmation_action_a),
votes (block_a),
node (node_a),
last_vote (std::chrono::steady_clock::now ()),
last_winner (block_a)
{
	assert (node_a.store.block_exists (transaction_a, block_a->hash ()));
	confirmed.clear ();
	compute_rep_votes (transaction_a);
}

void raze::election::compute_rep_votes (MDB_txn * transaction_a)
{
	node.wallets.foreach_representative (transaction_a, [this, transaction_a](raze::public_key const & pub_a, raze::raw_key const & prv_a) {
		auto vote (this->node.store.vote_generate (transaction_a, pub_a, prv_a, last_winner));
		this->votes.vote (vote.second);
	});
}

void raze::election::broadcast_winner ()
{
	{
		raze::transaction transaction (node.store.environment, nullptr, true);
		compute_rep_votes (transaction);
	}
	raze::transaction transaction_a (node.store.environment, nullptr, false);
	node.network.republish_block (transaction_a, last_winner);
}

raze::uint128_t raze::election::quorum_threshold (MDB_txn * transaction_a, raze::ledger & ledger_a)
{
	// Threshold over which unanimous voting implies confirmation
	return ledger_a.supply (transaction_a) / 2;
}

raze::uint128_t raze::election::minimum_threshold (MDB_txn * transaction_a, raze::ledger & ledger_a)
{
	// Minimum number of votes needed to change our ledger, underwhich we're probably disconnected
	return ledger_a.supply (transaction_a) / 16;
}

void raze::election::confirm_once (MDB_txn * transaction_a)
{
	if (!confirmed.test_and_set ())
	{
		auto tally_l (node.ledger.tally (transaction_a, votes));
		assert (tally_l.size () > 0);
		auto winner (tally_l.begin ());
		auto block_l (winner->second);
		auto exceeded_min_threshold = winner->first > minimum_threshold (transaction_a, node.ledger);
		if (!(*block_l == *last_winner))
		{
			if (exceeded_min_threshold)
			{
				auto node_l (node.shared ());
				node.background ([node_l, block_l]() {
					node_l->block_processor.process_receive_many (raze::block_processor_item (block_l, true));
				});
				last_winner = block_l;
			}
			else
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("Retaining block %1%") % last_winner->hash ().to_string ());
			}
		}
		auto winner_l (last_winner);
		auto node_l (node.shared ());
		auto confirmation_action_l (confirmation_action);
		node.background ([winner_l, confirmation_action_l, node_l, exceeded_min_threshold]() {
			node_l->process_confirmed (winner_l);
			confirmation_action_l (winner_l, exceeded_min_threshold);
		});
	}
}

bool raze::election::have_quorum (MDB_txn * transaction_a)
{
	auto tally_l (node.ledger.tally (transaction_a, votes));
	assert (tally_l.size () > 0);
	auto result (tally_l.begin ()->first > quorum_threshold (transaction_a, node.ledger));
	return result;
}

void raze::election::confirm_if_quorum (MDB_txn * transaction_a)
{
	auto quorum (have_quorum (transaction_a));
	if (quorum)
	{
		confirm_once (transaction_a);
	}
}

void raze::election::confirm_cutoff (MDB_txn * transaction_a)
{
	if (node.config.logging.vote_logging ())
	{
		BOOST_LOG (node.log) << boost::str (boost::format ("Vote tally weight %2% for root %1%") % votes.id.to_string () % last_winner->root ().to_string ());
		for (auto i (votes.rep_votes.begin ()), n (votes.rep_votes.end ()); i != n; ++i)
		{
			BOOST_LOG (node.log) << boost::str (boost::format ("%1% %2%") % i->first.to_account () % i->second->hash ().to_string ());
		}
	}
	confirm_once (transaction_a);
}

void raze::election::vote (std::shared_ptr<raze::vote> vote_a)
{
	node.network.republish_vote (last_vote, vote_a);
	last_vote = std::chrono::steady_clock::now ();
	raze::transaction transaction (node.store.environment, nullptr, true);
	//assert (node.store.vote_validate (transaction, vote_a).code != raze::vote_code::invalid);
	votes.vote (vote_a);
	confirm_if_quorum (transaction);
}

void raze::active_transactions::announce_votes ()
{
	std::vector<raze::block_hash> inactive;
	raze::transaction transaction (node.store.environment, nullptr, true);
	std::lock_guard<std::mutex> lock (mutex);

	{
		size_t announcements (0);
		auto i (roots.begin ());
		auto n (roots.end ());
		// Announce our decision for up to `announcements_per_interval' conflicts
		for (; i != n && announcements < announcements_per_interval; ++i)
		{
			auto election_l (i->election);
			node.background ([election_l]() { election_l->broadcast_winner (); });
			if (i->announcements >= contigious_announcements - 1)
			{
				// These blocks have reached the confirmation interval for forks
				i->election->confirm_cutoff (transaction);
				auto root_l (i->election->votes.id);
				inactive.push_back (root_l);
			}
			else
			{
				unsigned announcements;
				roots.modify (i, [&announcements](raze::conflict_info & info_a) {
					announcements = ++info_a.announcements;
				});
				// If more than one full announcement interval has passed and no one has voted on this block, we need to synchronize
				if (announcements > 1 && i->election->votes.rep_votes.size () <= 1)
				{
					node.bootstrap_initiator.bootstrap ();
				}
			}
		}
		// Mark remainder as 0 announcements sent
		// This could happen if there's a flood of forks, the network will resolve them in increasing root hash order
		// This is a DoS protection mechanism to rate-limit the amount of traffic for solving forks.
		for (; i != n; ++i)
		{
			// Reset announcement count for conflicts above announcement cutoff
			roots.modify (i, [](raze::conflict_info & info_a) {
				info_a.announcements = 0;
			});
		}
	}
	for (auto i (inactive.begin ()), n (inactive.end ()); i != n; ++i)
	{
		assert (roots.find (*i) != roots.end ());
		roots.erase (*i);
	}
	auto now (std::chrono::steady_clock::now ());
	auto node_l (node.shared ());
	node.alarm.add (now + std::chrono::milliseconds (announce_interval_ms), [node_l]() { node_l->active.announce_votes (); });
}

void raze::active_transactions::stop ()
{
	std::lock_guard<std::mutex> lock (mutex);
	roots.clear ();
}

bool raze::active_transactions::start (MDB_txn * transaction_a, std::shared_ptr<raze::block> block_a, std::function<void(std::shared_ptr<raze::block>, bool)> const & confirmation_action_a)
{
	std::lock_guard<std::mutex> lock (mutex);
	auto root (block_a->root ());
	auto existing (roots.find (root));
	if (existing == roots.end ())
	{
		auto election (std::make_shared<raze::election> (transaction_a, node, block_a, confirmation_action_a));
		roots.insert (raze::conflict_info{ root, election, 0 });
	}
	return existing != roots.end ();
}

// Validate a vote and apply it to the current election if one exists
void raze::active_transactions::vote (std::shared_ptr<raze::vote> vote_a)
{
	std::shared_ptr<raze::election> election;
	{
		std::lock_guard<std::mutex> lock (mutex);
		auto root (vote_a->block->root ());
		auto existing (roots.find (root));
		if (existing != roots.end ())
		{
			election = existing->election;
		}
	}
	if (election)
	{
		election->vote (vote_a);
	}
}

bool raze::active_transactions::active (raze::block const & block_a)
{
	std::lock_guard<std::mutex> lock (mutex);
	return roots.find (block_a.root ()) != roots.end ();
}

raze::active_transactions::active_transactions (raze::node & node_a) :
node (node_a)
{
}

int raze::node::store_version ()
{
	raze::transaction transaction (store.environment, nullptr, false);
	return store.version_get (transaction);
}

raze::thread_runner::thread_runner (boost::asio::io_service & service_a, unsigned service_threads_a)
{
	for (auto i (0); i < service_threads_a; ++i)
	{
		threads.push_back (std::thread ([&service_a]() {
			try
			{
				service_a.run ();
			}
			catch (...)
			{
				assert (false && "Unhandled service exception");
			}
		}));
	}
}

raze::thread_runner::~thread_runner ()
{
	join ();
}

void raze::thread_runner::join ()
{
	for (auto & i : threads)
	{
		if (i.joinable ())
		{
			i.join ();
		}
	}
}

void raze::add_node_options (boost::program_options::options_description & description_a)
{
	// clang-format off
	description_a.add_options ()
		("account_create", "Insert next deterministic key in to <wallet>")
		("account_get", "Get account number for the <key>")
		("account_key", "Get the public key for <account>")
		("vacuum", "Compact database. If data_path is missing, the database in data directory is compacted.")
		("data_path", boost::program_options::value<std::string> (), "Use the supplied path as the data directory")
		("diagnostics", "Run internal diagnostics")
		("key_create", "Generates a adhoc random keypair and prints it to stdout")
		("key_expand", "Derive public key and account number from <key>")
		("wallet_add_adhoc", "Insert <key> in to <wallet>")
		("wallet_create", "Creates a new wallet and prints the ID")
		("wallet_change_seed", "Changes seed for <wallet> to <key>")
		("wallet_decrypt_unsafe", "Decrypts <wallet> using <password>, !!THIS WILL PRINT YOUR PRIVATE KEY TO STDOUT!!")
		("wallet_destroy", "Destroys <wallet> and all keys it contains")
		("wallet_import", "Imports keys in <file> using <password> in to <wallet>")
		("wallet_list", "Dumps wallet IDs and public keys")
		("wallet_remove", "Remove <account> from <wallet>")
		("wallet_representative_get", "Prints default representative for <wallet>")
		("wallet_representative_set", "Set <account> as default representative for <wallet>")
		("vote_dump", "Dump most recent votes from representatives")
		("account", boost::program_options::value<std::string> (), "Defines <account> for other commands")
		("file", boost::program_options::value<std::string> (), "Defines <file> for other commands")
		("key", boost::program_options::value<std::string> (), "Defines the <key> for other commands, hex")
		("password", boost::program_options::value<std::string> (), "Defines <password> for other commands")
		("wallet", boost::program_options::value<std::string> (), "Defines <wallet> for other commands");
	// clang-format on
}

bool raze::handle_node_options (boost::program_options::variables_map & vm)
{
	auto result (false);
	boost::filesystem::path data_path = vm.count ("data_path") ? boost::filesystem::path (vm["data_path"].as<std::string> ()) : raze::working_path ();
	if (vm.count ("account_create"))
	{
		if (vm.count ("wallet") == 1)
		{
			raze::uint256_union wallet_id;
			if (!wallet_id.decode_hex (vm["wallet"].as<std::string> ()))
			{
				std::string password;
				if (vm.count ("password") > 0)
				{
					password = vm["password"].as<std::string> ();
				}
				inactive_node node (data_path);
				auto wallet (node.node->wallets.open (wallet_id));
				if (wallet != nullptr)
				{
					if (!wallet->enter_password (password))
					{
						raze::transaction transaction (wallet->store.environment, nullptr, true);
						auto pub (wallet->store.deterministic_insert (transaction));
						std::cout << boost::str (boost::format ("Account: %1%\n") % pub.to_account ());
					}
					else
					{
						std::cerr << "Invalid password\n";
						result = true;
					}
				}
				else
				{
					std::cerr << "Wallet doesn't exist\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "Invalid wallet id\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_add command requires one <wallet> option and one <key> option and optionally one <password> option\n";
			result = true;
		}
	}
	else if (vm.count ("account_get") > 0)
	{
		if (vm.count ("key") == 1)
		{
			raze::uint256_union pub;
			pub.decode_hex (vm["key"].as<std::string> ());
			std::cout << "Account: " << pub.to_account () << std::endl;
		}
		else
		{
			std::cerr << "account comand requires one <key> option\n";
			result = true;
		}
	}
	else if (vm.count ("account_key") > 0)
	{
		if (vm.count ("account") == 1)
		{
			raze::uint256_union account;
			account.decode_account (vm["account"].as<std::string> ());
			std::cout << "Hex: " << account.to_string () << std::endl;
		}
		else
		{
			std::cerr << "account_key command requires one <account> option\n";
			result = true;
		}
	}
	else if (vm.count ("vacuum") > 0)
	{
		try
		{
			auto vacuum_path = data_path / "vacuumed.ldb";
			auto source_path = data_path / "data.ldb";
			auto backup_path = data_path / "backup.vacuum.ldb";

			std::cout << "Vacuuming database copy in " << data_path << std::endl;
			std::cout << "This may take a while..." << std::endl;

			// Scope the node so the mdb environment gets cleaned up properly before
			// the original file is replaced with the vacuumed file.
			bool success = false;
			{
				inactive_node node (data_path);
				success = node.node->copy_with_compaction (vacuum_path);
			}

			if (success)
			{
				// Note that these throw on failure
				std::cout << "Finalizing" << std::endl;
				boost::filesystem::remove (backup_path);
				boost::filesystem::rename (source_path, backup_path);
				boost::filesystem::rename (vacuum_path, source_path);
				std::cout << "Vacuum completed" << std::endl;
			}
		}
		catch (const boost::filesystem::filesystem_error & ex)
		{
			std::cerr << "Vacuum failed during a file operation: " << ex.what () << std::endl;
		}
		catch (...)
		{
			std::cerr << "Vacuum failed" << std::endl;
		}
	}
	else if (vm.count ("diagnostics"))
	{
		inactive_node node (data_path);
		std::cout << "Testing hash function" << std::endl;
		raze::raw_key key;
		key.data.clear ();
		raze::send_block send (0, 0, 0, key, 0, 0);
		std::cout << "Testing key derivation function" << std::endl;
		raze::raw_key junk1;
		junk1.data.clear ();
		raze::uint256_union junk2 (0);
		raze::kdf kdf;
		kdf.phs (junk1, "", junk2);
		std::cout << "Dumping OpenCL information" << std::endl;
		bool error (false);
		raze::opencl_environment environment (error);
		if (!error)
		{
			environment.dump (std::cout);
			std::stringstream stream;
			environment.dump (stream);
			BOOST_LOG (node.logging.log) << stream.str ();
		}
		else
		{
			std::cout << "Error initializing OpenCL" << std::endl;
		}
	}
	else if (vm.count ("key_create"))
	{
		raze::keypair pair;
		std::cout << "Private: " << pair.prv.data.to_string () << std::endl
		          << "Public: " << pair.pub.to_string () << std::endl
		          << "Account: " << pair.pub.to_account () << std::endl;
	}
	else if (vm.count ("key_expand"))
	{
		if (vm.count ("key") == 1)
		{
			raze::uint256_union prv;
			prv.decode_hex (vm["key"].as<std::string> ());
			raze::uint256_union pub;
			ed25519_publickey (prv.bytes.data (), pub.bytes.data ());
			std::cout << "Private: " << prv.to_string () << std::endl
			          << "Public: " << pub.to_string () << std::endl
			          << "Account: " << pub.to_account () << std::endl;
		}
		else
		{
			std::cerr << "key_expand command requires one <key> option\n";
			result = true;
		}
	}
	else if (vm.count ("wallet_add_adhoc"))
	{
		if (vm.count ("wallet") == 1 && vm.count ("key") == 1)
		{
			raze::uint256_union wallet_id;
			if (!wallet_id.decode_hex (vm["wallet"].as<std::string> ()))
			{
				std::string password;
				if (vm.count ("password") > 0)
				{
					password = vm["password"].as<std::string> ();
				}
				inactive_node node (data_path);
				auto wallet (node.node->wallets.open (wallet_id));
				if (wallet != nullptr)
				{
					if (!wallet->enter_password (password))
					{
						raze::raw_key key;
						if (!key.data.decode_hex (vm["key"].as<std::string> ()))
						{
							raze::transaction transaction (wallet->store.environment, nullptr, true);
							wallet->store.insert_adhoc (transaction, key);
						}
						else
						{
							std::cerr << "Invalid key\n";
							result = true;
						}
					}
					else
					{
						std::cerr << "Invalid password\n";
						result = true;
					}
				}
				else
				{
					std::cerr << "Wallet doesn't exist\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "Invalid wallet id\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_add command requires one <wallet> option and one <key> option and optionally one <password> option\n";
			result = true;
		}
	}
	else if (vm.count ("wallet_change_seed"))
	{
		if (vm.count ("wallet") == 1 && vm.count ("key") == 1)
		{
			raze::uint256_union wallet_id;
			if (!wallet_id.decode_hex (vm["wallet"].as<std::string> ()))
			{
				std::string password;
				if (vm.count ("password") > 0)
				{
					password = vm["password"].as<std::string> ();
				}
				inactive_node node (data_path);
				auto wallet (node.node->wallets.open (wallet_id));
				if (wallet != nullptr)
				{
					if (!wallet->enter_password (password))
					{
						raze::raw_key key;
						if (!key.data.decode_hex (vm["key"].as<std::string> ()))
						{
							raze::transaction transaction (wallet->store.environment, nullptr, true);
							wallet->change_seed (transaction, key);
						}
						else
						{
							std::cerr << "Invalid key\n";
							result = true;
						}
					}
					else
					{
						std::cerr << "Invalid password\n";
						result = true;
					}
				}
				else
				{
					std::cerr << "Wallet doesn't exist\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "Invalid wallet id\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_add command requires one <wallet> option and one <key> option and optionally one <password> option\n";
			result = true;
		}
	}
	else if (vm.count ("wallet_create"))
	{
		inactive_node node (data_path);
		raze::keypair key;
		std::cout << key.pub.to_string () << std::endl;
		auto wallet (node.node->wallets.create (key.pub));
		wallet->enter_initial_password ();
	}
	else if (vm.count ("wallet_decrypt_unsafe"))
	{
		if (vm.count ("wallet") == 1)
		{
			std::string password;
			if (vm.count ("password") == 1)
			{
				password = vm["password"].as<std::string> ();
			}
			raze::uint256_union wallet_id;
			if (!wallet_id.decode_hex (vm["wallet"].as<std::string> ()))
			{
				inactive_node node (data_path);
				auto existing (node.node->wallets.items.find (wallet_id));
				if (existing != node.node->wallets.items.end ())
				{
					if (!existing->second->enter_password (password))
					{
						raze::transaction transaction (existing->second->store.environment, nullptr, false);
						raze::raw_key seed;
						existing->second->store.seed (seed, transaction);
						std::cout << boost::str (boost::format ("Seed: %1%\n") % seed.data.to_string ());
						for (auto i (existing->second->store.begin (transaction)), m (existing->second->store.end ()); i != m; ++i)
						{
							raze::account account (i->first.uint256 ());
							raze::raw_key key;
							auto error (existing->second->store.fetch (transaction, account, key));
							assert (!error);
							std::cout << boost::str (boost::format ("Pub: %1% Prv: %2%\n") % account.to_account () % key.data.to_string ());
						}
					}
					else
					{
						std::cerr << "Invalid password\n";
						result = true;
					}
				}
				else
				{
					std::cerr << "Wallet doesn't exist\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "Invalid wallet id\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_decrypt_unsafe requires one <wallet> option\n";
			result = true;
		}
	}
	else if (vm.count ("wallet_destroy"))
	{
		if (vm.count ("wallet") == 1)
		{
			raze::uint256_union wallet_id;
			if (!wallet_id.decode_hex (vm["wallet"].as<std::string> ()))
			{
				inactive_node node (data_path);
				if (node.node->wallets.items.find (wallet_id) != node.node->wallets.items.end ())
				{
					node.node->wallets.destroy (wallet_id);
				}
				else
				{
					std::cerr << "Wallet doesn't exist\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "Invalid wallet id\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_destroy requires one <wallet> option\n";
			result = true;
		}
	}
	else if (vm.count ("wallet_import"))
	{
		if (vm.count ("file") == 1)
		{
			std::string filename (vm["file"].as<std::string> ());
			std::ifstream stream;
			stream.open (filename.c_str ());
			if (!stream.fail ())
			{
				std::stringstream contents;
				contents << stream.rdbuf ();
				std::string password;
				if (vm.count ("password") == 1)
				{
					password = vm["password"].as<std::string> ();
				}
				if (vm.count ("wallet") == 1)
				{
					raze::uint256_union wallet_id;
					if (!wallet_id.decode_hex (vm["wallet"].as<std::string> ()))
					{
						inactive_node node (data_path);
						auto existing (node.node->wallets.items.find (wallet_id));
						if (existing != node.node->wallets.items.end ())
						{
							if (!existing->second->import (contents.str (), password))
							{
								result = false;
							}
							else
							{
								std::cerr << "Unable to import wallet\n";
								result = true;
							}
						}
						else
						{
							std::cerr << "Wallet doesn't exist\n";
							result = true;
						}
					}
					else
					{
						std::cerr << "Invalid wallet id\n";
						result = true;
					}
				}
				else
				{
					std::cerr << "wallet_import requires one <wallet> option\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "Unable to open <file>\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_import requires one <file> option\n";
			result = true;
		}
	}
	else if (vm.count ("wallet_list"))
	{
		inactive_node node (data_path);
		for (auto i (node.node->wallets.items.begin ()), n (node.node->wallets.items.end ()); i != n; ++i)
		{
			std::cout << boost::str (boost::format ("Wallet ID: %1%\n") % i->first.to_string ());
			raze::transaction transaction (i->second->store.environment, nullptr, false);
			for (auto j (i->second->store.begin (transaction)), m (i->second->store.end ()); j != m; ++j)
			{
				std::cout << raze::uint256_union (j->first.uint256 ()).to_account () << '\n';
			}
		}
	}
	else if (vm.count ("wallet_remove"))
	{
		if (vm.count ("wallet") == 1 && vm.count ("account") == 1)
		{
			inactive_node node (data_path);
			raze::uint256_union wallet_id;
			if (!wallet_id.decode_hex (vm["wallet"].as<std::string> ()))
			{
				auto wallet (node.node->wallets.items.find (wallet_id));
				if (wallet != node.node->wallets.items.end ())
				{
					raze::account account_id;
					if (!account_id.decode_account (vm["account"].as<std::string> ()))
					{
						raze::transaction transaction (wallet->second->store.environment, nullptr, true);
						auto account (wallet->second->store.find (transaction, account_id));
						if (account != wallet->second->store.end ())
						{
							wallet->second->store.erase (transaction, account_id);
						}
						else
						{
							std::cerr << "Account not found in wallet\n";
							result = true;
						}
					}
					else
					{
						std::cerr << "Invalid account id\n";
						result = true;
					}
				}
				else
				{
					std::cerr << "Wallet not found\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "Invalid wallet id\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_remove command requires one <wallet> and one <account> option\n";
			result = true;
		}
	}
	else if (vm.count ("wallet_representative_get"))
	{
		if (vm.count ("wallet") == 1)
		{
			raze::uint256_union wallet_id;
			if (!wallet_id.decode_hex (vm["wallet"].as<std::string> ()))
			{
				inactive_node node (data_path);
				auto wallet (node.node->wallets.items.find (wallet_id));
				if (wallet != node.node->wallets.items.end ())
				{
					raze::transaction transaction (wallet->second->store.environment, nullptr, false);
					auto representative (wallet->second->store.representative (transaction));
					std::cout << boost::str (boost::format ("Representative: %1%\n") % representative.to_account ());
				}
				else
				{
					std::cerr << "Wallet not found\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "Invalid wallet id\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_representative_get requires one <wallet> option\n";
			result = true;
		}
	}
	else if (vm.count ("wallet_representative_set"))
	{
		if (vm.count ("wallet") == 1)
		{
			if (vm.count ("account") == 1)
			{
				raze::uint256_union wallet_id;
				if (!wallet_id.decode_hex (vm["wallet"].as<std::string> ()))
				{
					raze::account account;
					if (!account.decode_account (vm["account"].as<std::string> ()))
					{
						inactive_node node (data_path);
						auto wallet (node.node->wallets.items.find (wallet_id));
						if (wallet != node.node->wallets.items.end ())
						{
							raze::transaction transaction (wallet->second->store.environment, nullptr, true);
							wallet->second->store.representative_set (transaction, account);
						}
						else
						{
							std::cerr << "Wallet not found\n";
							result = true;
						}
					}
					else
					{
						std::cerr << "Invalid account\n";
						result = true;
					}
				}
				else
				{
					std::cerr << "Invalid wallet id\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "wallet_representative_set requires one <account> option\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_representative_set requires one <wallet> option\n";
			result = true;
		}
	}
	else if (vm.count ("vote_dump") == 1)
	{
		inactive_node node (data_path);
		raze::transaction transaction (node.node->store.environment, nullptr, false);
		for (auto i (node.node->store.vote_begin (transaction)), n (node.node->store.vote_end ()); i != n; ++i)
		{
			bool error (false);
			raze::bufferstream stream (reinterpret_cast<uint8_t const *> (i->second.data ()), i->second.size ());
			auto vote (std::make_shared<raze::vote> (error, stream));
			assert (!error);
			std::cerr << boost::str (boost::format ("%1%\n") % vote->to_json ());
		}
	}
	else
	{
		result = true;
	}
	return result;
}

raze::inactive_node::inactive_node (boost::filesystem::path const & path) :
path (path),
service (boost::make_shared<boost::asio::io_service> ()),
alarm (*service),
work (1, nullptr)
{
	boost::filesystem::create_directories (path);
	logging.init (path);
	node = std::make_shared<raze::node> (init, *service, 24000, path, alarm, logging, work);
}

raze::inactive_node::~inactive_node ()
{
	node->stop ();
}

raze::port_mapping::port_mapping (raze::node & node_a) :
node (node_a),
devices (nullptr),
protocols ({ { { "TCP", 0, boost::asio::ip::address_v4::any (), 0 }, { "UDP", 0, boost::asio::ip::address_v4::any (), 0 } } }),
check_count (0),
on (false)
{
	urls = { 0 };
	data = { { 0 } };
}

void raze::port_mapping::start ()
{
	check_mapping_loop ();
}

void raze::port_mapping::refresh_devices ()
{
	if (raze::raze_network != raze::raze_networks::raze_test_network)
	{
		std::lock_guard<std::mutex> lock (mutex);
		int discover_error = 0;
		freeUPNPDevlist (devices);
		devices = upnpDiscover (2000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, false, 2, &discover_error);
		std::array<char, 64> local_address;
		local_address.fill (0);
		auto igd_error (UPNP_GetValidIGD (devices, &urls, &data, local_address.data (), sizeof (local_address)));
		if (igd_error == 1 || igd_error == 2)
		{
			boost::system::error_code ec;
			address = boost::asio::ip::address_v4::from_string (local_address.data (), ec);
		}
		if (check_count % 15 == 0)
		{
			BOOST_LOG (node.log) << boost::str (boost::format ("UPnP local address: %3%, discovery: %1%, IGD search: %2%") % discover_error % igd_error % local_address.data ());
			for (auto i (devices); i != nullptr; i = i->pNext)
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("UPnP device url: %1% st: %2% usn: %3%") % i->descURL % i->st % i->usn);
			}
		}
	}
}

void raze::port_mapping::refresh_mapping ()
{
	if (raze::raze_network != raze::raze_networks::raze_test_network)
	{
		std::lock_guard<std::mutex> lock (mutex);
		auto node_port (std::to_string (node.network.endpoint ().port ()));

		// Intentionally omitted: we don't map the RPC port because, unless RPC authentication was added, this would almost always be a security risk
		for (auto & protocol : protocols)
		{
			std::array<char, 6> actual_external_port;
			actual_external_port.fill (0);
			auto add_port_mapping_error (UPNP_AddAnyPortMapping (urls.controlURL, data.first.servicetype, node_port.c_str (), node_port.c_str (), address.to_string ().c_str (), nullptr, protocol.name, nullptr, std::to_string (mapping_timeout).c_str (), actual_external_port.data ()));
			if (check_count % 15 == 0)
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("UPnP %1% port mapping response: %2%, actual external port %5%") % protocol.name % add_port_mapping_error % 0 % 0 % actual_external_port.data ());
			}
			if (add_port_mapping_error == UPNPCOMMAND_SUCCESS)
			{
				protocol.external_port = std::atoi (actual_external_port.data ());
			}
			else
			{
				protocol.external_port = 0;
			}
		}
	}
}

int raze::port_mapping::check_mapping ()
{
	int result (3600);
	if (raze::raze_network != raze::raze_networks::raze_test_network)
	{
		// Long discovery time and fast setup/teardown make this impractical for testing
		std::lock_guard<std::mutex> lock (mutex);
		auto node_port (std::to_string (node.network.endpoint ().port ()));
		for (auto & protocol : protocols)
		{
			std::array<char, 64> int_client;
			std::array<char, 6> int_port;
			std::array<char, 16> remaining_mapping_duration;
			remaining_mapping_duration.fill (0);
			auto verify_port_mapping_error (UPNP_GetSpecificPortMappingEntry (urls.controlURL, data.first.servicetype, node_port.c_str (), protocol.name, nullptr, int_client.data (), int_port.data (), nullptr, nullptr, remaining_mapping_duration.data ()));
			if (verify_port_mapping_error == UPNPCOMMAND_SUCCESS)
			{
				protocol.remaining = result;
			}
			else
			{
				protocol.remaining = 0;
			}
			result = std::min (result, protocol.remaining);
			std::array<char, 64> external_address;
			external_address.fill (0);
			auto external_ip_error (UPNP_GetExternalIPAddress (urls.controlURL, data.first.servicetype, external_address.data ()));
			if (external_ip_error == UPNPCOMMAND_SUCCESS)
			{
				boost::system::error_code ec;
				protocol.external_address = boost::asio::ip::address_v4::from_string (external_address.data (), ec);
			}
			else
			{
				protocol.external_address = boost::asio::ip::address_v4::any ();
			}
			if (check_count % 15 == 0)
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("UPnP %3% mapping verification response: %1%, external ip response: %6%, external ip: %4%, internal ip: %5%, remaining lease: %2%") % verify_port_mapping_error % remaining_mapping_duration.data () % protocol.name % external_address.data () % address.to_string () % external_ip_error);
			}
		}
	}
	return result;
}

void raze::port_mapping::check_mapping_loop ()
{
	int wait_duration = check_timeout;
	refresh_devices ();
	if (devices != nullptr)
	{
		auto remaining (check_mapping ());
		// If the mapping is lost, refresh it
		if (remaining == 0)
		{
			refresh_mapping ();
		}
	}
	else
	{
		wait_duration = 300;
		if (check_count < 10)
		{
			BOOST_LOG (node.log) << boost::str (boost::format ("UPnP No IGD devices found"));
		}
	}
	++check_count;
	if (on)
	{
		auto node_l (node.shared ());
		node.alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (wait_duration), [node_l]() {
			node_l->port_mapping.check_mapping_loop ();
		});
	}
}

void raze::port_mapping::stop ()
{
	on = false;
	std::lock_guard<std::mutex> lock (mutex);
	for (auto & protocol : protocols)
	{
		if (protocol.external_port != 0)
		{
			// Be a good citizen for the router and shut down our mapping
			auto delete_error (UPNP_DeletePortMapping (urls.controlURL, data.first.servicetype, std::to_string (protocol.external_port).c_str (), protocol.name, address.to_string ().c_str ()));
			BOOST_LOG (node.log) << boost::str (boost::format ("Shutdown port mapping response: %1%") % delete_error);
		}
	}
	freeUPNPDevlist (devices);
	devices = nullptr;
}
