// Copyright (c)      2018, The Graft Project
// Copyright (c)      2018, The Loki Project
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

#include "checkpoint_vote_handler.h"
#include "rta_voting.h"
#include "cryptonote_config.h"
#include "graft_rta_config.h"
#include "cryptonote_core.h"
#include "blockchain_based_list.h"

#include "version.h"
#include "common/util.h"
#include "net/local_ip.h"
#include <boost/endian/conversion.hpp>


#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "CheckpointVoteHandler"

namespace rta
{

CheckpointVoteHandler::CheckpointVoteHandler(cryptonote::core& core)
  : m_core(core), m_last_checkpointed_height(0)
{
}

void CheckpointVoteHandler::init()
{
  m_last_checkpointed_height = 0;
}




void CheckpointVoteHandler::blockchain_detached(uint64_t height, bool by_pop_blocks)
{
  uint8_t hf_version                        = m_core.get_hard_fork_version(height);
  uint64_t const REORG_SAFETY_BUFFER_BLOCKS = (hf_version >= cryptonote::network_version_18_checkpointing)
      ? config::graft::REORG_SAFETY_BUFFER_BLOCKS_POST_HF18
      : config::graft::REORG_SAFETY_BUFFER_BLOCKS_PRE_HF18;
  
  if (m_last_checkpointed_height >= height + REORG_SAFETY_BUFFER_BLOCKS)
  {
    if (!by_pop_blocks)
    {
      LOG_ERROR("The blockchain was detached to height: " << height << ", but quorum cop has already processed votes for checkpointing up to " << m_last_checkpointed_height);
      LOG_ERROR("This implies a reorg occured that was over " << REORG_SAFETY_BUFFER_BLOCKS << ". This should rarely happen! Please report this to the devs.");
    }
    m_last_checkpointed_height = height - (height % config::graft::CHECKPOINT_INTERVAL);
  }
  
  m_vote_pool.remove_expired_votes(height);
}


bool CheckpointVoteHandler::handle_vote(const checkpoint_vote &vote, cryptonote::vote_verification_context &vvc)
{
  vvc = {};
  
  if (!verify_vote_age(vote, m_core.get_current_blockchain_height(), vvc))
    return false;

  // get checkpointing sample;
  cryptonote::BlockchainBasedList::supernode_array checkpoint_sample;
  if (!m_core.get_stake_tx_processor().build_checkpointing_sample(vote.block_hash, vote.block_height, checkpoint_sample)) {
    MERROR("Failed to build checkpointing sample for height: " << vote.block_height);
    vvc.m_invalid_block_height = true;
    return false;
  }
  
  if (!verify_vote_signature(m_core.get_hard_fork_version(vote.block_height), vote, vvc, *quorum))
    return false;

  std::vector<pool_vote_entry> votes = m_vote_pool.add_pool_vote_if_unique(vote, vvc);
  if (!vvc.m_added_to_pool) // NOTE: Not unique vote
    return true;

  return handle_checkpoint_vote(m_core, vote, votes, *quorum);
      
  
}



void CheckpointVoteHandler::set_votes_relayed(std::vector<checkpoint_vote> const &relayed_votes)
{
  m_vote_pool.set_relayed(relayed_votes);
}

std::vector<checkpoint_vote> CheckpointVoteHandler::get_relayable_votes(uint64_t current_height, uint8_t hf_version, bool quorum_relay)
{
  return m_vote_pool.get_relayable_votes(current_height, hf_version, quorum_relay);
}

int find_index_in_quorum_group(std::vector<crypto::public_key> const &group, crypto::public_key const &my_pubkey)
{
  int result = -1;
  auto it = std::find(group.begin(), group.end(), my_pubkey);
  if (it == group.end()) return result;
  result = std::distance(group.begin(), it);
  return result;
}

void CheckpointVoteHandler::process_quorums(cryptonote::block const &block)
{
  uint8_t const hf_version = block.major_version;
  if (hf_version < cryptonote::network_version_18_checkpointing)
    return;
  
  uint64_t const REORG_SAFETY_BUFFER_BLOCKS = (hf_version >= cryptonote::network_version_12_checkpointing)
      ? REORG_SAFETY_BUFFER_BLOCKS_POST_HF12
      : REORG_SAFETY_BUFFER_BLOCKS_PRE_HF12;
  auto my_keys = m_core.get_service_node_keys();
  bool voting_enabled = my_keys && m_core.is_service_node(my_keys->pub, /*require_active=*/true);
  
  uint64_t const height        = cryptonote::get_block_height(block);
  uint64_t const latest_height = std::max(m_core.get_current_blockchain_height(), m_core.get_target_blockchain_height());
  if (latest_height < VOTE_LIFETIME)
    return;
  
  uint64_t const start_voting_from_height = latest_height - VOTE_LIFETIME;
  if (height < start_voting_from_height)
    return;
  
  service_nodes::quorum_type const max_quorum_type = service_nodes::max_quorum_type_for_hf(hf_version);
  bool tested_myself_once_per_block                = false;
  
  time_t start_time   = m_core.get_start_time();
  time_t const now    = time(nullptr);
  int const live_time = (now - start_time);
  for (int i = 0; i <= (int)max_quorum_type; i++)
  {
    quorum_type const type = static_cast<quorum_type>(i);
    
#if defined(LOKI_ENABLE_INTEGRATION_TEST_HOOKS)
    if (integration_test::state.disable_checkpoint_quorum && type == quorum_type::checkpointing) continue;
    if (integration_test::state.disable_obligation_quorum && type == quorum_type::obligations) continue;
#endif
    
    switch(type)
    {
    default:
    {
      assert("Unhandled quorum type " == 0);
      LOG_ERROR("Unhandled quorum type with value: " << (int)type);
    } break;
      
    case quorum_type::obligations:
    {
      
      m_obligations_height = std::max(m_obligations_height, start_voting_from_height);
      for (; m_obligations_height < (height - REORG_SAFETY_BUFFER_BLOCKS); m_obligations_height++)
      {
        uint8_t const obligations_height_hf_version = m_core.get_hard_fork_version(m_obligations_height);
        if (obligations_height_hf_version < cryptonote::network_version_9_service_nodes) continue;
        
        // NOTE: Count checkpoints for other nodes, irrespective of being
        // a service node or not for statistics. Also count checkpoints
        // before the minimum lifetime for same purposes, note, we still
        // don't vote for the first 2 hours so this is purely cosmetic
        if (obligations_height_hf_version >= cryptonote::network_version_12_checkpointing)
        {
          auto quorum = m_core.get_quorum(quorum_type::checkpointing, m_obligations_height);
          std::vector<cryptonote::block> blocks;
          if (quorum && m_core.get_blocks(m_obligations_height, 1, blocks))
          {
            cryptonote::block const &block = blocks[0];
            if (start_time < static_cast<ptrdiff_t>(block.timestamp)) // NOTE: If we started up before receiving the block, we likely have the voting information, if not we probably don't.
            {
              uint64_t quorum_height = offset_testing_quorum_height(quorum_type::checkpointing, m_obligations_height);
              for (size_t index_in_quorum = 0; index_in_quorum < quorum->validators.size(); index_in_quorum++)
              {
                crypto::public_key const &key = quorum->validators[index_in_quorum];
                m_core.record_checkpoint_vote(
                      key,
                      quorum_height,
                      m_vote_pool.received_checkpoint_vote(m_obligations_height, index_in_quorum));
              }
            }
          }
        }
        
        // NOTE: Wait at least 2 hours before we're allowed to vote so that we collect necessary voting information from people on the network
        bool alive_for_min_time = live_time >= MIN_TIME_IN_S_BEFORE_VOTING;
        if (!alive_for_min_time)
          continue;
        
        if (!my_keys)
          continue;
        
        auto quorum = m_core.get_quorum(quorum_type::obligations, m_obligations_height);
        if (!quorum)
        {
          // TODO(loki): Fatal error
          LOG_ERROR("Obligations quorum for height: " << m_obligations_height << " was not cached in daemon!");
          continue;
        }
        
        if (quorum->workers.empty()) continue;
        int index_in_group = voting_enabled ? find_index_in_quorum_group(quorum->validators, my_keys->pub) : -1;
        if (index_in_group >= 0)
        {
          //
          // NOTE: I am in the quorum
          //
          auto worker_states = m_core.get_service_node_list_state(quorum->workers);
          auto worker_it = worker_states.begin();
          CRITICAL_REGION_LOCAL(m_lock);
          int good = 0, total = 0;
          for (size_t node_index = 0; node_index < quorum->workers.size(); ++worker_it, ++node_index)
          {
            // If the SN no longer exists then it'll be omitted from the worker_states vector,
            // so if the elements don't line up skip ahead.
            while (worker_it->pubkey != quorum->workers[node_index] && node_index < quorum->workers.size())
              node_index++;
            if (node_index == quorum->workers.size())
              break;
            total++;
            
            const auto &node_key = worker_it->pubkey;
            const auto &info = *worker_it->info;
            
            if (!info.can_be_voted_on(m_obligations_height))
              continue;
            
            auto test_results = check_service_node(obligations_height_hf_version, node_key, info);
            bool passed       = test_results.passed();
            
            new_state vote_for_state;
            if (passed) {
              if (info.is_decommissioned()) {
                vote_for_state = new_state::recommission;
                LOG_PRINT_L2("Decommissioned service node " << quorum->workers[node_index] << " is now passing required checks; voting to recommission");
              } else if (!test_results.single_ip) {
                // Don't worry about this if the SN is getting recommissioned (above) -- it'll
                // already reenter at the bottom.
                vote_for_state = new_state::ip_change_penalty;
                LOG_PRINT_L2("Service node " << quorum->workers[node_index] << " was observed with multiple IPs recently; voting to reset reward position");
              } else {
                good++;
                continue;
              }
              
            }
            else {
              int64_t credit = calculate_decommission_credit(info, latest_height);
              
              if (info.is_decommissioned()) {
                if (credit >= 0) {
                  LOG_PRINT_L2("Decommissioned service node "
                               << quorum->workers[node_index]
                               << " is still not passing required checks, but has remaining credit (" << credit
                               << " blocks); abstaining (to leave decommissioned)");
                  continue;
                }
                
                LOG_PRINT_L2("Decommissioned service node " << quorum->workers[node_index] << " has no remaining credit; voting to deregister");
                vote_for_state = new_state::deregister; // Credit ran out!
              } else {
                if (credit >= DECOMMISSION_MINIMUM) {
                  vote_for_state = new_state::decommission;
                  LOG_PRINT_L2("Service node "
                               << quorum->workers[node_index]
                               << " has stopped passing required checks, but has sufficient earned credit (" << credit << " blocks) to avoid deregistration; voting to decommission");
                } else {
                  vote_for_state = new_state::deregister;
                  LOG_PRINT_L2("Service node "
                               << quorum->workers[node_index]
                               << " has stopped passing required checks, but does not have sufficient earned credit ("
                               << credit << " blocks, " << DECOMMISSION_MINIMUM
                               << " required) to decommission; voting to deregister");
                }
              }
            }
            
            quorum_vote_t vote = service_nodes::make_state_change_vote(m_obligations_height, static_cast<uint16_t>(index_in_group), node_index, vote_for_state, *my_keys);
            cryptonote::vote_verification_context vvc;
            if (!handle_vote(vote, vvc))
              LOG_ERROR("Failed to add state change vote; reason: " << print_vote_verification_context(vvc, &vote));
          }
          if (good > 0)
            LOG_PRINT_L2(good << " of " << total << " service nodes are active and passing checks; no state change votes required");
        }
        else if (!tested_myself_once_per_block && (find_index_in_quorum_group(quorum->workers, my_keys->pub) >= 0))
        {
          // NOTE: Not in validating quorum , check if we're the ones
          // being tested. If so, check if we would be decommissioned
          // based on _our_ data and if so, report it to the user so they
          // know about it.
          
          const auto states_array = m_core.get_service_node_list_state({my_keys->pub});
          if (states_array.size())
          {
            const auto &info = *states_array[0].info;
            if (info.can_be_voted_on(m_obligations_height))
            {
              tested_myself_once_per_block = true;
              auto my_test_results         = check_service_node(obligations_height_hf_version, my_keys->pub, info);
              if (info.is_active())
              {
                if (!my_test_results.passed())
                {
                  // NOTE: Don't warn uptime proofs if the daemon is just
                  // recently started and is candidate for testing (i.e.
                  // restarting the daemon)
                  if (!my_test_results.uptime_proved && live_time < LOKI_HOUR(1))
                    continue;
                  
                  LOG_PRINT_L0("Service Node (yours) is active but is not passing tests for quorum: " << m_obligations_height);
                  LOG_PRINT_L0(my_test_results.why());
                }
              }
              else if (info.is_decommissioned())
              {
                LOG_PRINT_L0("Service Node (yours) is currently decommissioned and being tested in quorum: " << m_obligations_height);
                LOG_PRINT_L0(my_test_results.why());
              }
            }
          }
        }
      }
    }
      break;
      
    case quorum_type::checkpointing:
    {
      if (voting_enabled)
      {
        uint64_t start_checkpointing_height = start_voting_from_height;
        if ((start_checkpointing_height % CHECKPOINT_INTERVAL) > 0)
          start_checkpointing_height += (CHECKPOINT_INTERVAL - (start_checkpointing_height % CHECKPOINT_INTERVAL));
        
        m_last_checkpointed_height = std::max(start_checkpointing_height, m_last_checkpointed_height);
        
        for (;
             m_last_checkpointed_height <= height;
             m_last_checkpointed_height += CHECKPOINT_INTERVAL)
        {
          uint8_t checkpointed_height_hf_version = m_core.get_hard_fork_version(m_last_checkpointed_height);
          if (checkpointed_height_hf_version <= cryptonote::network_version_11_infinite_staking)
            continue;
          
          if (m_last_checkpointed_height < REORG_SAFETY_BUFFER_BLOCKS)
            continue;
          
          auto quorum = m_core.get_quorum(quorum_type::checkpointing, m_last_checkpointed_height);
          if (!quorum)
          {
            // TODO(loki): Fatal error
            LOG_ERROR("Checkpoint quorum for height: " << m_last_checkpointed_height << " was not cached in daemon!");
            continue;
          }
          
          int index_in_group = find_index_in_quorum_group(quorum->validators, my_keys->pub);
          if (index_in_group <= -1) continue;
          
          //
          // NOTE: I am in the quorum, handle checkpointing
          //
          crypto::hash block_hash = m_core.get_block_id_by_height(m_last_checkpointed_height);
          quorum_vote_t vote = make_checkpointing_vote(checkpointed_height_hf_version, block_hash, m_last_checkpointed_height, static_cast<uint16_t>(index_in_group), *my_keys);
          cryptonote::vote_verification_context vvc = {};
          if (!handle_vote(vote, vvc))
            LOG_ERROR("Failed to add checkpoint vote; reason: " << print_vote_verification_context(vvc, &vote));
        }
      }
    }
      break;
      
    case quorum_type::blink:
      break;
    }
  }
}

bool CheckpointVoteHandler::block_added(const cryptonote::block& block, const std::vector<cryptonote::transaction>& txs, cryptonote::checkpoint_t const * /*checkpoint*/)
{
  process_quorums(block);
  uint64_t const height = cryptonote::get_block_height(block) + 1; // chain height = new top block height + 1
  m_vote_pool.remove_expired_votes(height);
  m_vote_pool.remove_used_votes(txs, block.major_version);
  
  // These feels out of place here because the hook system sucks: TODO replace it with
  // std::function hooks instead.
  m_core.update_lmq_sns();
  
  return true;
}

static bool handle_obligations_vote(cryptonote::core &core, const quorum_vote_t& vote, const std::vector<pool_vote_entry>& votes, const quorum& quorum)
{
  if (votes.size() < STATE_CHANGE_MIN_VOTES_TO_CHANGE_STATE)
  {
    LOG_PRINT_L2("Don't have enough votes yet to submit a state change transaction: have " << votes.size() << " of " << STATE_CHANGE_MIN_VOTES_TO_CHANGE_STATE << " required");
    return true;
  }
  
  uint8_t const hf_version = core.get_blockchain_storage().get_current_hard_fork_version();
  
  // NOTE: Verify state change is still valid or have we processed some other state change already that makes it invalid
  {
    crypto::public_key const &service_node_pubkey = quorum.workers[vote.state_change.worker_index];
    auto service_node_infos = core.get_service_node_list_state({service_node_pubkey});
    if (!service_node_infos.size() ||
        !service_node_infos[0].info->can_transition_to_state(hf_version, vote.block_height, vote.state_change.state))
      // NOTE: Vote is valid but is invalidated because we cannot apply the change to a service node or it is not on the network anymore
      //       So don't bother generating a state change tx.
      return true;
  }
  
  cryptonote::tx_extra_service_node_state_change state_change{vote.state_change.state, vote.block_height, vote.state_change.worker_index};
  state_change.votes.reserve(votes.size());
  
  for (const auto &pool_vote : votes)
    state_change.votes.emplace_back(pool_vote.vote.signature, pool_vote.vote.index_in_group);
  
  cryptonote::transaction state_change_tx{};
  if (cryptonote::add_service_node_state_change_to_tx_extra(state_change_tx.extra, state_change, hf_version))
  {
    state_change_tx.version = cryptonote::transaction::get_max_version_for_hf(hf_version);
    state_change_tx.type    = cryptonote::txtype::state_change;
    
    cryptonote::tx_verification_context tvc{};
    cryptonote::blobdata const tx_blob = cryptonote::tx_to_blob(state_change_tx);
    
    bool result = core.handle_incoming_tx(tx_blob, tvc, cryptonote::tx_pool_options::new_tx());
    if (!result || tvc.m_verifivation_failed)
    {
      LOG_PRINT_L1("A full state change tx for height: " << vote.block_height <<
                   " and service node: " << vote.state_change.worker_index <<
                   " could not be verified and was not added to the memory pool, reason: " <<
                   print_tx_verification_context(tvc, &state_change_tx));
      return false;
    }
  }
  else
    LOG_PRINT_L1("Failed to add state change to tx extra for height: "
                 << vote.block_height << " and service node: " << vote.state_change.worker_index);
  
  return true;
}

static bool handle_checkpoint_vote(cryptonote::core& core, const quorum_vote_t& vote, const std::vector<pool_vote_entry>& votes, const quorum& quorum)
{
  if (votes.size() < CHECKPOINT_MIN_VOTES)
  {
    LOG_PRINT_L2("Don't have enough votes yet to submit a checkpoint: have " << votes.size() << " of " << CHECKPOINT_MIN_VOTES << " required");
    return true;
  }
  MDEBUG(__FUNCTION__);
  cryptonote::checkpoint_t checkpoint{};
  cryptonote::Blockchain &blockchain = core.get_blockchain_storage();
  
  // NOTE: Multiple network threads are going to try and update the
  // checkpoint, blockchain.update_checkpoint does NOT do any
  // validation- that is done here since we want to keep code for
  // converting votes to data suitable for the DB in service node land.
  
  // So then, multiple threads can race to update the checkpoint. One
  // thread could retrieve an outdated checkpoint whilst another has
  // already updated it. i.e. we could replace a checkpoint with lesser
  // votes prematurely. The actual update in the DB is an atomic
  // operation, but this check and validation step is NOT, taking the
  // lock here makes it so.
  
  std::unique_lock<cryptonote::Blockchain> lock{blockchain};
  
  bool update_checkpoint;
  if (blockchain.get_checkpoint(vote.block_height, checkpoint) &&
      checkpoint.block_hash == vote.checkpoint.block_hash)
  {
    MDEBUG(__FUNCTION__ << " existing checkpoint.");
    update_checkpoint = false;
    if (checkpoint.signatures.size() != service_nodes::CHECKPOINT_QUORUM_SIZE)
    {
      checkpoint.signatures.reserve(service_nodes::CHECKPOINT_QUORUM_SIZE);
      std::sort(checkpoint.signatures.begin(),
                checkpoint.signatures.end(),
                [](service_nodes::voter_to_signature const &lhs, service_nodes::voter_to_signature const &rhs) {
        return lhs.voter_index < rhs.voter_index;
      });
      
      for (pool_vote_entry const &pool_vote : votes)
      {
        auto it = std::lower_bound(checkpoint.signatures.begin(),
                                   checkpoint.signatures.end(),
                                   pool_vote,
                                   [](voter_to_signature const &lhs, pool_vote_entry const &vote) {
            return lhs.voter_index < vote.vote.index_in_group;
      });
        
        if (it == checkpoint.signatures.end() ||
            pool_vote.vote.index_in_group != it->voter_index)
        {
          update_checkpoint = true;
          checkpoint.signatures.insert(it, voter_to_signature(pool_vote.vote));
        }
      }
    }
  }
  else
  {
    MDEBUG(__FUNCTION__ << " new checkpoint.");
    update_checkpoint = true;
    checkpoint = make_empty_service_node_checkpoint(vote.checkpoint.block_hash, vote.block_height);
    
    checkpoint.signatures.reserve(votes.size());
    for (pool_vote_entry const &pool_vote : votes)
      checkpoint.signatures.push_back(voter_to_signature(pool_vote.vote));
  }
  
  if (update_checkpoint)
    blockchain.update_checkpoint(checkpoint);
  
  return true;
}

bool CheckpointVoteHandler::handle_vote(quorum_vote_t const &vote, cryptonote::vote_verification_context &vvc)
{
  vvc = {};
  if (!verify_vote_age(vote, m_core.get_current_blockchain_height(), vvc))
    return false;
  
  std::shared_ptr<const quorum> quorum = m_core.get_quorum(vote.type, vote.block_height);
  if (!quorum)
  {
    vvc.m_invalid_block_height = true;
    return false;
  }
  
  if (!verify_vote_signature(m_core.get_hard_fork_version(vote.block_height), vote, vvc, *quorum))
    return false;
  
  std::vector<pool_vote_entry> votes = m_vote_pool.add_pool_vote_if_unique(vote, vvc);
  if (!vvc.m_added_to_pool) // NOTE: Not unique vote
    return true;
  
  bool result = true;
  switch(vote.type)
  {
  default:
  {
    LOG_PRINT_L1("Unhandled vote type with value: " << (int)vote.type);
    assert("Unhandled vote type" == 0);
    return false;
  };
    
  case quorum_type::obligations:
    result &= handle_obligations_vote(m_core, vote, votes, *quorum);
    break;
    
  case quorum_type::checkpointing:
    result &= handle_checkpoint_vote(m_core, vote, votes, *quorum);
    break;
  }
  return result;
}

// Calculate the decommission credit for a service node.  If the SN is current decommissioned this
// returns the number of blocks remaining in the credit; otherwise this is the number of currently
// accumulated blocks.
int64_t CheckpointVoteHandler::calculate_decommission_credit(const service_node_info &info, uint64_t current_height)
{
  // If currently decommissioned, we need to know how long it was up before being decommissioned;
  // otherwise we need to know how long since it last become active until now (or 0 if not staked
  // yet).
  int64_t blocks_up;
  if (!info.is_fully_funded())
    blocks_up = 0;
  if (info.is_decommissioned()) // decommissioned; the negative of active_since_height tells us when the period leading up to the current decommission started
    blocks_up = int64_t(info.last_decommission_height) - (-info.active_since_height);
  else
    blocks_up = int64_t(current_height) - int64_t(info.active_since_height);
  
  // Now we calculate the credit earned from being up for `blocks_up` blocks
  int64_t credit = 0;
  if (blocks_up >= 0) {
    credit = blocks_up * DECOMMISSION_CREDIT_PER_DAY / BLOCKS_EXPECTED_IN_HOURS(24);
    
    if (info.decommission_count <= info.is_decommissioned()) // Has never been decommissioned (or is currently in the first decommission), so add initial starting credit
      credit += DECOMMISSION_INITIAL_CREDIT;
    if (credit > DECOMMISSION_MAX_CREDIT)
      credit = DECOMMISSION_MAX_CREDIT; // Cap the available decommission credit blocks if above the max
  }
  
  // If currently decommissioned, remove any used credits used for the current downtime
  if (info.is_decommissioned())
    credit -= int64_t(current_height) - int64_t(info.last_decommission_height);
  
  return credit;
}

uint64_t quorum_checksum(const std::vector<crypto::public_key> &pubkeys, size_t offset) {
  constexpr size_t KEY_BYTES = sizeof(crypto::public_key);
  
  // Calculate a checksum by reading bytes 0-7 from the first pubkey as a little-endian uint64_t,
  // then reading 1-8 from the second pubkey, 2-9 from the third, and so on, and adding all the
  // uint64_t values together.  If we get to 25 we wrap the read around the end and keep going.
  uint64_t sum = 0;
  alignas(uint64_t) std::array<char, sizeof(uint64_t)> local;
  for (auto &pk : pubkeys) {
    offset %= KEY_BYTES;
    auto *pkdata = reinterpret_cast<const char *>(&pk);
    if (offset <= KEY_BYTES - sizeof(uint64_t))
      std::memcpy(local.data(), pkdata + offset, sizeof(uint64_t));
    else {
      size_t prewrap = KEY_BYTES - offset;
      std::memcpy(local.data(), pkdata + offset, prewrap);
      std::memcpy(local.data() + prewrap, pkdata, sizeof(uint64_t) - prewrap);
    }
    sum += boost::endian::little_to_native(*reinterpret_cast<uint64_t *>(local.data()));
    ++offset;
  }
  return sum;
}

}