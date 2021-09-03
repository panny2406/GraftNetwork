// Copyright (c) 2014-2019, The Monero Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#pragma once
#include <array>
#include <atomic>
#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/thread.hpp>
#include <boost/optional/optional_fwd.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/uuid/uuid.hpp>
#include <functional>
#include <utility>
#include <vector>

#include "cryptonote_config.h"
#include "warnings.h"
#include "net/abstract_tcp_server2.h"
#include "net/levin_protocol_handler.h"
#include "net/levin_protocol_handler_async.h"
#include "p2p_protocol_defs.h"
#include "storages/levin_abstract_invoke2.h"
#include "net_peerlist.h"
#include "math_helper.h"
#include "net_node_common.h"
#include "net/enums.h"
#include "net/fwd.h"
#include "common/command_line.h"
#include "net/jsonrpc_structs.h"
#include "storages/http_abstract_invoke.h"

#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <iomanip>

PUSH_WARNINGS
DISABLE_VS_WARNINGS(4355)

namespace nodetool
{
  using Uuid = boost::uuids::uuid;

  using namespace std::literals;
  struct proxy
  {
    proxy()
      : max_connections(-1),
        address(),
        zone(epee::net_utils::zone::invalid)
    {}

    std::int64_t max_connections;
    boost::asio::ip::tcp::endpoint address;
    epee::net_utils::zone zone;
  };

  struct anonymous_inbound
  {
    anonymous_inbound()
      : max_connections(-1),
        local_ip(),
        local_port(),
        our_address(),
        default_remote()
    {}

    std::int64_t max_connections;
    std::string local_ip;
    std::string local_port;
    epee::net_utils::network_address our_address;
    epee::net_utils::network_address default_remote;
  };

  boost::optional<std::vector<proxy>> get_proxies(const boost::program_options::variables_map& vm);
  boost::optional<std::vector<anonymous_inbound>> get_anonymous_inbounds(const boost::program_options::variables_map& vm);

  //! \return True if `commnd` is filtered (ignored/dropped) for `address`
  bool is_filtered_command(epee::net_utils::network_address const& address, int command);

  // hides boost::future and chrono stuff from mondo template file
  boost::optional<boost::asio::ip::tcp::socket>
  socks_connect_internal(const std::atomic<bool>& stop_signal, boost::asio::io_service& service, const boost::asio::ip::tcp::endpoint& proxy, const epee::net_utils::network_address& remote);


  template<class base_type>
  struct p2p_connection_context_t: base_type //t_payload_net_handler::connection_context //public net_utils::connection_context_base
  {
    p2p_connection_context_t(): peer_id(0), support_flags(0), m_in_timedsync(false) {}

    peerid_type peer_id;
    uint32_t support_flags;
    bool m_in_timedsync;
  };

  struct local_supernode {
    local_supernode(std::string host, uint64_t port, std::string uri) : http_host(std::move(host)), http_port(port), uri(std::move(uri)) {
        client.set_server(http_host, std::to_string(http_port), {});
    }

    void update(const std::string &new_host, uint64_t new_port, const std::string &new_uri) {
        if (new_host != http_host || new_port != http_port) {
            if (client.is_connected()) client.disconnect();
            client.set_server(new_host, std::to_string(new_port), {});
            http_host = new_host;
            http_port = new_port;
            uri = new_uri;
        }
    }

    std::string http_host;
    uint64_t http_port;
    std::string uri;
    epee::net_utils::http::http_simple_client client;
  };

  template<class t_payload_net_handler>
  class node_server: public epee::levin::levin_commands_handler<p2p_connection_context_t<typename t_payload_net_handler::connection_context> >,
                     public i_p2p_endpoint<typename t_payload_net_handler::connection_context>,
                     public epee::net_utils::i_connection_filter
  {
    struct by_conn_id{};
    struct by_peer_id{};
    struct by_addr{};

    typedef p2p_connection_context_t<typename t_payload_net_handler::connection_context> p2p_connection_context;

    typedef COMMAND_HANDSHAKE_T<typename t_payload_net_handler::payload_type> COMMAND_HANDSHAKE;
    typedef COMMAND_TIMED_SYNC_T<typename t_payload_net_handler::payload_type> COMMAND_TIMED_SYNC;

    typedef epee::net_utils::boosted_tcp_server<epee::levin::async_protocol_handler<p2p_connection_context>> net_server;

    struct network_zone;
    using connect_func = boost::optional<p2p_connection_context>(network_zone&, epee::net_utils::network_address const&, epee::net_utils::ssl_support_t);

    struct config_t
    {
      config_t()
        : m_net_config(),
          m_peer_id(crypto::rand<uint64_t>()),
          m_support_flags(0)
      {}

      network_config m_net_config;
      uint64_t m_peer_id;
      uint32_t m_support_flags;
    };
    typedef epee::misc_utils::struct_init<config_t> config;

    struct network_zone
    {
      network_zone()
        : m_connect(nullptr),
          m_net_server(epee::net_utils::e_connection_type_P2P),
          m_bind_ip(),
          m_bind_ipv6_address(),
          m_port(),
          m_port_ipv6(),
          m_our_address(),
          m_peerlist(),
          m_config{},
          m_proxy_address(),
          m_current_number_of_out_peers(0),
          m_current_number_of_in_peers(0),
          m_can_pingback(false)
      {
        set_config_defaults();
      }

      network_zone(boost::asio::io_service& public_service)
        : m_connect(nullptr),
          m_net_server(public_service, epee::net_utils::e_connection_type_P2P),
          m_bind_ip(),
          m_bind_ipv6_address(),
          m_port(),
          m_port_ipv6(),
          m_our_address(),
          m_peerlist(),
          m_config{},
          m_proxy_address(),
          m_current_number_of_out_peers(0),
          m_current_number_of_in_peers(0),
          m_can_pingback(false)
      {
        set_config_defaults();
      }

      connect_func* m_connect;
      net_server m_net_server;
      std::string m_bind_ip;
      std::string m_bind_ipv6_address;
      std::string m_port;
      std::string m_port_ipv6;
      epee::net_utils::network_address m_our_address; // in anonymity networks
      peerlist_manager m_peerlist;
      config m_config;
      boost::asio::ip::tcp::endpoint m_proxy_address;
      std::atomic<unsigned int> m_current_number_of_out_peers;
      std::atomic<unsigned int> m_current_number_of_in_peers;
      bool m_can_pingback;

    private:
      void set_config_defaults() noexcept
      {
        // at this moment we have a hardcoded config
        m_config.m_net_config.handshake_interval = P2P_DEFAULT_HANDSHAKE_INTERVAL;
        m_config.m_net_config.packet_max_size = P2P_DEFAULT_PACKET_MAX_SIZE;
        m_config.m_net_config.config_id = 0;
        m_config.m_net_config.connection_timeout = P2P_DEFAULT_CONNECTION_TIMEOUT;
        m_config.m_net_config.ping_connection_timeout = P2P_DEFAULT_PING_CONNECTION_TIMEOUT;
        m_config.m_net_config.send_peerlist_sz = P2P_DEFAULT_PEERS_IN_HANDSHAKE;
        m_config.m_support_flags = 0; // only set in public zone
      }
    };

    enum igd_t
    {
      no_igd,
      igd,
      delayed_igd,
    };

  public:
    typedef t_payload_net_handler payload_net_handler;

    node_server(t_payload_net_handler& payload_handler)
      : m_payload_handler(payload_handler),
        m_external_port(0),
        m_rpc_port(0),
        m_allow_local_ip(false),
        m_hide_my_port(false),
        m_igd(no_igd),
        m_offline(false),
        is_closing(false),
        m_network_id()
    {}
    virtual ~node_server();

    static void init_options(boost::program_options::options_description& desc);

    bool run();
    network_zone& add_zone(epee::net_utils::zone zone);
    bool init(const boost::program_options::variables_map& vm);
    bool deinit();
    bool send_stop_signal();
    uint32_t get_this_peer_port(){return m_listening_port;}
    t_payload_net_handler& get_payload_object();

    // debug functions
    bool log_peerlist();
    bool log_connections();

    // These functions only return information for the "public" zone
    virtual uint64_t get_public_connections_count();
    size_t get_public_outgoing_connections_count();
    size_t get_public_white_peers_count();
    size_t get_public_gray_peers_count();
    void get_public_peerlist(std::vector<peerlist_entry>& gray, std::vector<peerlist_entry>& white);
    size_t get_zone_count() const { return m_network_zones.size(); }

    void change_max_out_public_peers(size_t count);
    void change_max_in_public_peers(size_t count);
    virtual bool block_host(const epee::net_utils::network_address &adress, time_t seconds = P2P_IP_BLOCKTIME);
    virtual bool unblock_host(const epee::net_utils::network_address &address);
    virtual bool block_subnet(const epee::net_utils::ipv4_network_subnet &subnet, time_t seconds = P2P_IP_BLOCKTIME);
    virtual bool unblock_subnet(const epee::net_utils::ipv4_network_subnet &subnet);
    virtual bool is_host_blocked(const epee::net_utils::network_address &address, time_t *seconds) { CRITICAL_REGION_LOCAL(m_blocked_hosts_lock); return !is_remote_host_allowed(address, seconds); }
    virtual std::map<std::string, time_t> get_blocked_hosts() { CRITICAL_REGION_LOCAL(m_blocked_hosts_lock); return m_blocked_hosts; }

    // Graft/RTA methods to be called from RPC handlers

    /*!
     * \brief do_supernode_announce - posts supernode announce to p2p network
     * \param req
     */
    void do_supernode_announce(const cryptonote::COMMAND_RPC_SUPERNODE_ANNOUNCE::request &req);
    /*!
     * \brief do_broadcast - posts broadcast message to p2p network
     * \param req
     */
    void do_broadcast(const cryptonote::COMMAND_RPC_BROADCAST::request &req);
    /*!
     * \brief do_multicast - posts multicast message to p2p network
     * \param req
     */
    void do_multicast(const cryptonote::COMMAND_RPC_MULTICAST::request &req);
    /*!
     * \brief do_unicast - posts unicast message to p2p network
     * \param req
     */
    void do_unicast(const cryptonote::COMMAND_RPC_UNICAST::request &req);

    std::vector<cryptonote::route_data> get_tunnels() const;

    virtual std::map<epee::net_utils::ipv4_network_subnet, time_t> get_blocked_subnets() { CRITICAL_REGION_LOCAL(m_blocked_hosts_lock); return m_blocked_subnets; }

    virtual void add_used_stripe_peer(const typename t_payload_net_handler::connection_context &context);
    virtual void remove_used_stripe_peer(const typename t_payload_net_handler::connection_context &context);
    virtual void clear_used_stripe_peers();

  private:
    const std::vector<std::string> m_seed_nodes_list =
    {
      // TODO(graft): "seeds.graft.network"
    };

    bool islimitup=false;
    bool islimitdown=false;

    typedef COMMAND_REQUEST_STAT_INFO_T<typename t_payload_net_handler::stat_info> COMMAND_REQUEST_STAT_INFO;

    CHAIN_LEVIN_INVOKE_MAP2(p2p_connection_context); //move levin_commands_handler interface invoke(...) callbacks into invoke map
    CHAIN_LEVIN_NOTIFY_MAP2(p2p_connection_context); //move levin_commands_handler interface notify(...) callbacks into nothing

    BEGIN_INVOKE_MAP2(node_server)
      // TODO: graft: remove
      HANDLE_NOTIFY_T2(COMMAND_SUPERNODE_ANNOUNCE, &node_server::handle_supernode_announce)
      HANDLE_NOTIFY_T2(COMMAND_BROADCAST, &node_server::handle_broadcast)
      HANDLE_NOTIFY_T2(COMMAND_MULTICAST, &node_server::handle_multicast)
      HANDLE_NOTIFY_T2(COMMAND_UNICAST, &node_server::handle_unicast)

      HANDLE_INVOKE_T2(COMMAND_HANDSHAKE, &node_server::handle_handshake)
      HANDLE_INVOKE_T2(COMMAND_TIMED_SYNC, &node_server::handle_timed_sync)
      HANDLE_INVOKE_T2(COMMAND_PING, &node_server::handle_ping)
      HANDLE_INVOKE_T2(COMMAND_REQUEST_STAT_INFO, &node_server::handle_get_stat_info)
      HANDLE_INVOKE_T2(COMMAND_REQUEST_NETWORK_STATE, &node_server::handle_get_network_state)
      HANDLE_INVOKE_T2(COMMAND_REQUEST_PEER_ID, &node_server::handle_get_peer_id)
      HANDLE_INVOKE_T2(COMMAND_REQUEST_SUPPORT_FLAGS, &node_server::handle_get_support_flags)
      if (is_filtered_command(context.m_remote_address, command))
        return LEVIN_ERROR_CONNECTION_HANDLER_NOT_DEFINED;

      CHAIN_INVOKE_MAP_TO_OBJ_FORCE_CONTEXT(m_payload_handler, typename t_payload_net_handler::connection_context&)
    END_INVOKE_MAP2()

    enum PeerType { anchor = 0, white, gray };

    //----------------- helper functions ------------------------------------------------
    bool multicast_send(int command, const std::string &data, const std::list<std::string> &addresses,
                        const std::list<peerid_type> &exclude_peerids = std::list<peerid_type>());
    uint64_t get_max_hop(const std::list<std::string> &addresses);
    std::list<std::string> get_routes();

    // sometimes supernode gets very busy so it doesn't respond within 1 second, increasing timeout to 3s
    static constexpr size_t SUPERNODE_HTTP_TIMEOUT_MILLIS = 3 * 1000;
    template<class request_struct>
    int post_request_to_supernode(local_supernode &supernode, const std::string &method, const typename request_struct::request &body,
                                  const std::string &endpoint = std::string())
    {
        boost::value_initialized<epee::json_rpc::request<typename request_struct::request> > init_req;
        epee::json_rpc::request<typename request_struct::request>& req = static_cast<epee::json_rpc::request<typename request_struct::request> &>(init_req);
        req.jsonrpc = "2.0";
        req.id = 0;
        req.method = method;
        req.params = body;

        std::string uri = "/" + method;
        if (!endpoint.empty())
        {
            uri = endpoint;
        }
        typename request_struct::response resp = AUTO_VAL_INIT(resp);
        bool r = epee::net_utils::invoke_http_json(supernode.uri + uri,
                                                   req, resp, supernode.client,
                                                   std::chrono::milliseconds(size_t(SUPERNODE_HTTP_TIMEOUT_MILLIS)), "POST");
        if (!r || resp.status == 0)
        {
            return 0;
        }
        return 1;
    }

    template<class request_struct>
    int post_request_to_supernodes(const std::string &method, const typename request_struct::request &body,
                                   const std::string &endpoint = std::string())
    {
        int ret = 0;
        for (auto &supernode : m_supernodes)
            ret += post_request_to_supernode<request_struct>(supernode.second, method, body, endpoint);
        return ret;
    }

    void remove_old_request_cache();

    //----------------- commands handlers ----------------------------------------------
    int handle_supernode_announce(int command, typename COMMAND_SUPERNODE_ANNOUNCE::request& arg, p2p_connection_context& context);
    int handle_broadcast(int command, typename COMMAND_BROADCAST::request &arg, p2p_connection_context &context);
    int handle_multicast(int command, typename COMMAND_MULTICAST::request &arg, p2p_connection_context &context);
    int handle_unicast(int command, typename COMMAND_UNICAST::request &arg, p2p_connection_context &context);
    int handle_handshake(int command, typename COMMAND_HANDSHAKE::request& arg, typename COMMAND_HANDSHAKE::response& rsp, p2p_connection_context& context);
    int handle_timed_sync(int command, typename COMMAND_TIMED_SYNC::request& arg, typename COMMAND_TIMED_SYNC::response& rsp, p2p_connection_context& context);
    int handle_ping(int command, COMMAND_PING::request& arg, COMMAND_PING::response& rsp, p2p_connection_context& context);
    int handle_get_stat_info(int command, typename COMMAND_REQUEST_STAT_INFO::request& arg, typename COMMAND_REQUEST_STAT_INFO::response& rsp, p2p_connection_context& context);
    int handle_get_network_state(int command, COMMAND_REQUEST_NETWORK_STATE::request& arg, COMMAND_REQUEST_NETWORK_STATE::response& rsp, p2p_connection_context& context);
    int handle_get_peer_id(int command, COMMAND_REQUEST_PEER_ID::request& arg, COMMAND_REQUEST_PEER_ID::response& rsp, p2p_connection_context& context);
    int handle_get_support_flags(int command, COMMAND_REQUEST_SUPPORT_FLAGS::request& arg, COMMAND_REQUEST_SUPPORT_FLAGS::response& rsp, p2p_connection_context& context);
    bool init_config();
    bool make_default_peer_id();
    bool make_default_config();
    bool store_config();
    bool check_trust(const proof_of_trust& tr, epee::net_utils::zone zone_type);


    //----------------- levin_commands_handler -------------------------------------------------------------
    virtual void on_connection_new(p2p_connection_context& context);
    virtual void on_connection_close(p2p_connection_context& context);
    virtual void callback(p2p_connection_context& context);
    //----------------- i_p2p_endpoint -------------------------------------------------------------
    virtual bool relay_notify_to_list(int command, const epee::span<const uint8_t> data_buff, std::vector<std::pair<epee::net_utils::zone, boost::uuids::uuid>> connections);
    virtual bool invoke_command_to_peer(int command, const epee::span<const uint8_t> req_buff, std::string& resp_buff, const epee::net_utils::connection_context_base& context);
    virtual bool invoke_notify_to_peer(int command, const epee::span<const uint8_t> req_buff, const epee::net_utils::connection_context_base& context);
    virtual bool drop_connection(const epee::net_utils::connection_context_base& context);
    virtual void request_callback(const epee::net_utils::connection_context_base& context);
    virtual void for_each_connection(std::function<bool(typename t_payload_net_handler::connection_context&, peerid_type, uint32_t)> f);
    virtual bool for_connection(const boost::uuids::uuid&, std::function<bool(typename t_payload_net_handler::connection_context&, peerid_type, uint32_t)> f);
    virtual bool add_host_fail(const epee::net_utils::network_address &address);
    // added, non virtual
    /*!
     * \brief relay_notify    - send command to remote connection
     * \param command         - command
     * \param data_buff       - data buffer to send
     * \param connection_id   - connection id
     * \return                - true on success
     */
    bool relay_notify(int command, const std::string& data_buff, const boost::uuids::uuid& connection_id);
    //----------------- i_connection_filter  --------------------------------------------------------
    virtual bool is_remote_host_allowed(const epee::net_utils::network_address &address, time_t *t = NULL);
    //-----------------------------------------------------------------------------------------------
    bool parse_peer_from_string(epee::net_utils::network_address& pe, const std::string& node_addr, uint16_t default_port = 0);
    bool handle_command_line(
        const boost::program_options::variables_map& vm
      );
    bool idle_worker();
    bool handle_remote_peerlist(const std::vector<peerlist_entry>& peerlist, time_t local_time, const epee::net_utils::connection_context_base& context);
    bool get_local_node_data(basic_node_data& node_data, const network_zone& zone);
    //bool get_local_handshake_data(handshake_data& hshd);

    bool merge_peerlist_with_local(const std::vector<peerlist_entry>& bs);
    bool fix_time_delta(std::vector<peerlist_entry>& local_peerlist, time_t local_time, int64_t& delta);

    bool connections_maker();
    bool peer_sync_idle_maker();
    bool do_handshake_with_peer(peerid_type& pi, p2p_connection_context& context, bool just_take_peerlist = false);
    bool do_peer_timed_sync(const epee::net_utils::connection_context_base& context, peerid_type peer_id);

    bool make_new_connection_from_anchor_peerlist(const std::vector<anchor_peerlist_entry>& anchor_peerlist);
    bool make_new_connection_from_peerlist(network_zone& zone, bool use_white_list);
    bool try_to_connect_and_handshake_with_new_peer(const epee::net_utils::network_address& na, bool just_take_peerlist = false, uint64_t last_seen_stamp = 0, PeerType peer_type = white, uint64_t first_seen_stamp = 0);
    size_t get_random_index_with_fixed_probability(size_t max_index);
    bool is_peer_used(const peerlist_entry& peer);
    bool is_peer_used(const anchor_peerlist_entry& peer);
    bool is_addr_connected(const epee::net_utils::network_address& peer);
    void add_upnp_port_mapping_impl(uint32_t port, bool ipv6=false);
    void add_upnp_port_mapping_v4(uint32_t port);
    void add_upnp_port_mapping_v6(uint32_t port);
    void add_upnp_port_mapping(uint32_t port, bool ipv4=true, bool ipv6=false);
    void delete_upnp_port_mapping_impl(uint32_t port, bool ipv6=false);
    void delete_upnp_port_mapping_v4(uint32_t port);
    void delete_upnp_port_mapping_v6(uint32_t port);
    void delete_upnp_port_mapping(uint32_t port);
    template<class t_callback>
    bool try_ping(basic_node_data& node_data, p2p_connection_context& context, const t_callback &cb);
    bool try_get_support_flags(const p2p_connection_context& context, std::function<void(p2p_connection_context&, const uint32_t&)> f);
    bool make_expected_connections_count(network_zone& zone, PeerType peer_type, size_t expected_connections);
    void cache_connect_fail_info(const epee::net_utils::network_address& addr);
    bool is_addr_recently_failed(const epee::net_utils::network_address& addr);
    bool is_priority_node(const epee::net_utils::network_address& na);
    std::set<std::string> get_seed_nodes(cryptonote::network_type nettype) const;
    bool connect_to_seed();
    bool find_connection_id_by_peer(const peerlist_entry &pe, boost::uuids::uuid &conn_id);
    template <class Container>
    bool connect_to_peerlist(const Container& peers);

    template <class Container>
    bool parse_peers_and_add_to_container(const boost::program_options::variables_map& vm, const command_line::arg_descriptor<std::vector<std::string> > & arg, Container& container);

    bool set_max_out_peers(network_zone& zone, int64_t max);
    bool set_max_in_peers(network_zone& zone, int64_t max);
    bool set_tos_flag(const boost::program_options::variables_map& vm, int limit);

    bool set_rate_up_limit(const boost::program_options::variables_map& vm, int64_t limit);
    bool set_rate_down_limit(const boost::program_options::variables_map& vm, int64_t limit);
    bool set_rate_limit(const boost::program_options::variables_map& vm, int64_t limit);

    bool has_too_many_connections(const epee::net_utils::network_address &address);
    uint64_t get_connections_count();
    size_t get_incoming_connections_count();
    size_t get_incoming_connections_count(network_zone&);
    size_t get_outgoing_connections_count();
    size_t get_outgoing_connections_count(network_zone&);

    bool check_connection_and_handshake_with_peer(const epee::net_utils::network_address& na, uint64_t last_seen_stamp);
    bool gray_peerlist_housekeeping();
    bool check_incoming_connections();

    void kill() { ///< will be called e.g. from deinit()
      _info("Killing the net_node");
      is_closing = true;
      if(mPeersLoggerThread != nullptr)
        mPeersLoggerThread->join(); // make sure the thread finishes
      _info("Joined extra background net_node threads");
    }


    //debug functions
    std::string print_connections_container();


  public:

    void set_rpc_port(uint16_t rpc_port)
    {
      m_rpc_port = rpc_port;
    }

    void reset_peer_handshake_timer()
    {
      m_peer_handshake_idle_maker_interval.reset();
    }

    void add_supernode(const std::string& addr, const std::string& url)
    {
        epee::net_utils::http::url_content parsed{};
        bool ret = epee::net_utils::parse_url(url, parsed);
        boost::lock_guard<boost::recursive_mutex> guard(m_supernode_lock);
        auto it = m_supernodes.find(addr);
        if (!ret) {
            if (it != m_supernodes.end()) m_supernodes.erase(it);
        } else if (it == m_supernodes.end()) {
            LOG_PRINT_L0("Adding supernode " << addr << " at " << parsed.host << ":" << parsed.port);
            m_supernodes.emplace(std::piecewise_construct,
                    std::forward_as_tuple(addr),
                    std::forward_as_tuple(std::move(parsed.host), parsed.port, std::move(parsed.uri)));
        } else {
            it->second.update(parsed.host, parsed.port, parsed.uri);
        }
    }

    std::vector<std::string> get_supernodes_addresses() {
        boost::lock_guard<boost::recursive_mutex> guard(m_supernode_lock);
        std::vector<std::string> addrs;
        addrs.reserve(m_supernodes.size());
        for (auto &sn : m_supernodes) {
            addrs.push_back(sn.first);
        }
        return addrs;
    }

    std::string join_supernodes_addresses(const std::string &joiner = " ") {
        std::ostringstream s;
        bool first = true;
        for (auto &addr : get_supernodes_addresses()) {
            if (first) first = false;
            else s << joiner;
            s << addr;
        }
        return s.str();
    }

    bool remove_supernode(const std::string &addr) {
        boost::lock_guard<boost::recursive_mutex> guard(m_supernode_lock);
        return m_supernodes.erase(addr);
    }

    void reset_supernodes() {
        boost::lock_guard<boost::recursive_mutex> guard(m_supernode_lock);
        m_supernodes.clear();
    }

    bool notify_peer_list(int command, const std::string& buf, const std::vector<peerlist_entry>& peers_to_send, bool try_connect = false);

    void send_stakes_to_supernode();
    void send_blockchain_based_list_to_supernode(uint64_t last_received_block_height);

    uint64_t get_announce_bytes_in() const { return m_announce_bytes_in; }
    uint64_t get_announce_bytes_out() const { return m_announce_bytes_out; }
    uint64_t get_broadcast_bytes_in() const { return m_broadcast_bytes_in; }
    uint64_t get_broadcast_bytes_out() const { return m_broadcast_bytes_out; }
    uint64_t get_multicast_bytes_in() const { return m_multicast_bytes_in; }
    uint64_t get_multicast_bytes_out() const { return m_multicast_bytes_out; }

  private:
    void handle_stakes_update(uint64_t block_number, const cryptonote::StakeTransactionProcessor::supernode_stake_array& stakes);
    void handle_blockchain_based_list_update(uint64_t block_number, const cryptonote::StakeTransactionProcessor::supernode_tier_array& tiers);

  private:
    std::multimap<int, std::string> m_supernode_requests_timestamps;
    std::set<std::string> m_supernode_requests_cache;
    std::map<std::string, nodetool::supernode_route> m_supernode_routes;
    std::unordered_map<std::string, local_supernode> m_supernodes;
    boost::recursive_mutex m_supernode_lock;
    boost::recursive_mutex m_request_cache_lock;
    std::vector<epee::net_utils::network_address> m_custom_seed_nodes;

    std::string m_config_folder;

    bool m_have_address;
    bool m_first_connection_maker_call;
    uint32_t m_listening_port;
    uint32_t m_listening_port_ipv6;
    uint32_t m_external_port;
    uint16_t m_rpc_port;
    bool m_allow_local_ip;
    bool m_hide_my_port;
    igd_t m_igd;
    bool m_offline;
    bool m_use_ipv6;
    bool m_require_ipv4;
    std::atomic<bool> is_closing;
    std::unique_ptr<boost::thread> mPeersLoggerThread;
    //critical_section m_connections_lock;
    //connections_indexed_container m_connections;

    t_payload_net_handler& m_payload_handler;
    peerlist_storage m_peerlist_storage;

    epee::math_helper::periodic_task m_peer_handshake_idle_maker_interval{std::chrono::seconds{P2P_DEFAULT_HANDSHAKE_INTERVAL}};
    epee::math_helper::periodic_task m_connections_maker_interval{1s};
    epee::math_helper::periodic_task m_peerlist_store_interval{30min};
    epee::math_helper::periodic_task m_gray_peerlist_housekeeping_interval{1min};
    epee::math_helper::periodic_task m_incoming_connections_interval{1h};

    uint64_t m_last_stat_request_time;
    std::list<epee::net_utils::network_address>   m_priority_peers;
    std::vector<epee::net_utils::network_address> m_exclusive_peers;
    std::vector<epee::net_utils::network_address> m_seed_nodes;
    bool m_fallback_seed_nodes_added;
    std::vector<nodetool::peerlist_entry> m_command_line_peers;
    uint64_t m_peer_livetime;
    //keep connections to initiate some interactions
    net_server m_net_server;
    boost::uuids::uuid m_network_id;

    static boost::optional<p2p_connection_context> public_connect(network_zone&, epee::net_utils::network_address const&, epee::net_utils::ssl_support_t);
    static boost::optional<p2p_connection_context> socks_connect(network_zone&, epee::net_utils::network_address const&, epee::net_utils::ssl_support_t);


    /* A `std::map` provides constant iterators and key/value pointers even with
    inserts/erases to _other_ elements. This makes the configuration step easier
    since references can safely be stored on the stack. Do not insert/erase
    after configuration and before destruction, lock safety would need to be
    added. `std::map::operator[]` WILL insert! */
    std::map<epee::net_utils::zone, network_zone> m_network_zones;


    std::map<std::string, time_t> m_conn_fails_cache;
    epee::critical_section m_conn_fails_cache_lock;

    epee::critical_section m_blocked_hosts_lock; // for both hosts and subnets
    std::map<std::string, time_t> m_blocked_hosts;
    std::map<epee::net_utils::ipv4_network_subnet, time_t> m_blocked_subnets;

    epee::critical_section m_host_fails_score_lock;
    std::map<std::string, uint64_t> m_host_fails_score;

    boost::mutex m_used_stripe_peers_mutex;
    std::array<std::list<epee::net_utils::network_address>, 1 << CRYPTONOTE_PRUNING_LOG_STRIPES> m_used_stripe_peers;

    boost::uuids::uuid m_network_id;
    cryptonote::network_type m_nettype;
    // traffic counters
    std::atomic<uint64_t> m_announce_bytes_in {0};
    std::atomic<uint64_t> m_announce_bytes_out {0};
    std::atomic<uint64_t> m_broadcast_bytes_in {0};
    std::atomic<uint64_t> m_broadcast_bytes_out {0};
    std::atomic<uint64_t> m_multicast_bytes_in {0};
    std::atomic<uint64_t> m_multicast_bytes_out {0};

    epee::net_utils::ssl_support_t m_ssl_support;
  };


    const int64_t default_limit_up = P2P_DEFAULT_LIMIT_RATE_UP;      // kB/s
    const int64_t default_limit_down = P2P_DEFAULT_LIMIT_RATE_DOWN;  // kB/s
    extern const command_line::arg_descriptor<std::string> arg_p2p_bind_ip;
    extern const command_line::arg_descriptor<std::string> arg_p2p_bind_ipv6_address;
    extern const command_line::arg_descriptor<std::string, false, true, 2> arg_p2p_bind_port;
    extern const command_line::arg_descriptor<std::string, false, true, 2> arg_p2p_bind_port_ipv6;
    extern const command_line::arg_descriptor<bool>        arg_p2p_use_ipv6;
    extern const command_line::arg_descriptor<bool>        arg_p2p_require_ipv4;
    extern const command_line::arg_descriptor<uint32_t>    arg_p2p_external_port;
    extern const command_line::arg_descriptor<bool>        arg_p2p_allow_local_ip;
    extern const command_line::arg_descriptor<std::vector<std::string> > arg_p2p_add_peer;
    extern const command_line::arg_descriptor<std::vector<std::string> > arg_p2p_add_priority_node;
    extern const command_line::arg_descriptor<std::vector<std::string> > arg_p2p_add_exclusive_node;
    extern const command_line::arg_descriptor<std::vector<std::string> > arg_p2p_seed_node;
    extern const command_line::arg_descriptor<std::vector<std::string> > arg_proxy;
    extern const command_line::arg_descriptor<std::vector<std::string> > arg_anonymous_inbound;
    extern const command_line::arg_descriptor<bool> arg_p2p_hide_my_port;
    extern const command_line::arg_descriptor<bool> arg_no_sync;

    extern const command_line::arg_descriptor<bool>        arg_no_igd;
    extern const command_line::arg_descriptor<std::string> arg_igd;
    extern const command_line::arg_descriptor<bool>        arg_offline;
    extern const command_line::arg_descriptor<int64_t>     arg_out_peers;
    extern const command_line::arg_descriptor<int64_t>     arg_in_peers;
    extern const command_line::arg_descriptor<int> arg_tos_flag;

    extern const command_line::arg_descriptor<int64_t> arg_limit_rate_up;
    extern const command_line::arg_descriptor<int64_t> arg_limit_rate_down;
    extern const command_line::arg_descriptor<int64_t> arg_limit_rate;
}

POP_WARNINGS

