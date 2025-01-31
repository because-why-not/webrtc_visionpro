/*
 *  Copyright (c) 2021 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "net/dcsctp/tx/outstanding_data.h"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "net/dcsctp/common/math.h"
#include "net/dcsctp/common/sequence_numbers.h"
#include "net/dcsctp/public/types.h"
#include "rtc_base/logging.h"

namespace dcsctp {
using ::webrtc::Timestamp;

// The number of times a packet must be NACKed before it's retransmitted.
// See https://tools.ietf.org/html/rfc4960#section-7.2.4
constexpr uint8_t kNumberOfNacksForRetransmission = 3;

// Returns how large a chunk will be, serialized, carrying the data
size_t OutstandingData::GetSerializedChunkSize(const Data& data) const {
  return RoundUpTo4(data_chunk_header_size_ + data.size());
}

void OutstandingData::Item::Ack() {
  if (lifecycle_ != Lifecycle::kAbandoned) {
    lifecycle_ = Lifecycle::kActive;
  }
  ack_state_ = AckState::kAcked;
}

OutstandingData::Item::NackAction OutstandingData::Item::Nack(
    bool retransmit_now) {
  ack_state_ = AckState::kNacked;
  ++nack_count_;
  if (!should_be_retransmitted() && !is_abandoned() &&
      (retransmit_now || nack_count_ >= kNumberOfNacksForRetransmission)) {
    // Nacked enough times - it's considered lost.
    if (num_retransmissions_ < *max_retransmissions_) {
      lifecycle_ = Lifecycle::kToBeRetransmitted;
      return NackAction::kRetransmit;
    }
    Abandon();
    return NackAction::kAbandon;
  }
  return NackAction::kNothing;
}

void OutstandingData::Item::MarkAsRetransmitted() {
  lifecycle_ = Lifecycle::kActive;
  ack_state_ = AckState::kUnacked;

  nack_count_ = 0;
  ++num_retransmissions_;
}

void OutstandingData::Item::Abandon() {
  RTC_DCHECK(!expires_at_.IsPlusInfinity() ||
             max_retransmissions_ != MaxRetransmits::NoLimit());
  lifecycle_ = Lifecycle::kAbandoned;
}

bool OutstandingData::Item::has_expired(Timestamp now) const {
  return expires_at_ <= now;
}

bool OutstandingData::IsConsistent() const {
  size_t actual_outstanding_bytes = 0;
  size_t actual_outstanding_items = 0;

  std::set<UnwrappedTSN> combined_to_be_retransmitted;
  combined_to_be_retransmitted.insert(to_be_retransmitted_.begin(),
                                      to_be_retransmitted_.end());
  combined_to_be_retransmitted.insert(to_be_fast_retransmitted_.begin(),
                                      to_be_fast_retransmitted_.end());

  std::set<UnwrappedTSN> actual_combined_to_be_retransmitted;
  for (const auto& [tsn, item] : outstanding_data_) {
    if (item.is_outstanding()) {
      actual_outstanding_bytes += GetSerializedChunkSize(item.data());
      ++actual_outstanding_items;
    }

    if (item.should_be_retransmitted()) {
      actual_combined_to_be_retransmitted.insert(tsn);
    }
  }

  return actual_outstanding_bytes == outstanding_bytes_ &&
         actual_outstanding_items == outstanding_items_ &&
         actual_combined_to_be_retransmitted == combined_to_be_retransmitted;
}

void OutstandingData::AckChunk(AckInfo& ack_info,
                               std::map<UnwrappedTSN, Item>::iterator iter) {
  if (!iter->second.is_acked()) {
    size_t serialized_size = GetSerializedChunkSize(iter->second.data());
    ack_info.bytes_acked += serialized_size;
    if (iter->second.is_outstanding()) {
      outstanding_bytes_ -= serialized_size;
      --outstanding_items_;
    }
    if (iter->second.should_be_retransmitted()) {
      RTC_DCHECK(to_be_fast_retransmitted_.find(iter->first) ==
                 to_be_fast_retransmitted_.end());
      to_be_retransmitted_.erase(iter->first);
    }
    iter->second.Ack();
    ack_info.highest_tsn_acked =
        std::max(ack_info.highest_tsn_acked, iter->first);
  }
}

OutstandingData::AckInfo OutstandingData::HandleSack(
    UnwrappedTSN cumulative_tsn_ack,
    rtc::ArrayView<const SackChunk::GapAckBlock> gap_ack_blocks,
    bool is_in_fast_recovery) {
  OutstandingData::AckInfo ack_info(cumulative_tsn_ack);
  // Erase all items up to cumulative_tsn_ack.
  RemoveAcked(cumulative_tsn_ack, ack_info);

  // ACK packets reported in the gap ack blocks
  AckGapBlocks(cumulative_tsn_ack, gap_ack_blocks, ack_info);

  // NACK and possibly mark for retransmit chunks that weren't acked.
  NackBetweenAckBlocks(cumulative_tsn_ack, gap_ack_blocks, is_in_fast_recovery,
                       ack_info);

  RTC_DCHECK(IsConsistent());
  return ack_info;
}

void OutstandingData::RemoveAcked(UnwrappedTSN cumulative_tsn_ack,
                                  AckInfo& ack_info) {
  auto first_unacked = outstanding_data_.upper_bound(cumulative_tsn_ack);

  for (auto iter = outstanding_data_.begin(); iter != first_unacked; ++iter) {
    AckChunk(ack_info, iter);
    if (iter->second.lifecycle_id().IsSet()) {
      RTC_DCHECK(iter->second.data().is_end);
      if (iter->second.is_abandoned()) {
        ack_info.abandoned_lifecycle_ids.push_back(iter->second.lifecycle_id());
      } else {
        ack_info.acked_lifecycle_ids.push_back(iter->second.lifecycle_id());
      }
    }
  }

  outstanding_data_.erase(outstanding_data_.begin(), first_unacked);
  last_cumulative_tsn_ack_ = cumulative_tsn_ack;
  stream_reset_breakpoint_tsns_.erase(stream_reset_breakpoint_tsns_.begin(),
                                      stream_reset_breakpoint_tsns_.upper_bound(
                                          cumulative_tsn_ack.next_value()));
}

void OutstandingData::AckGapBlocks(
    UnwrappedTSN cumulative_tsn_ack,
    rtc::ArrayView<const SackChunk::GapAckBlock> gap_ack_blocks,
    AckInfo& ack_info) {
  // Mark all non-gaps as ACKED (but they can't be removed) as (from RFC)
  // "SCTP considers the information carried in the Gap Ack Blocks in the
  // SACK chunk as advisory.". Note that when NR-SACK is supported, this can be
  // handled differently.

  for (auto& block : gap_ack_blocks) {
    auto start = outstanding_data_.lower_bound(
        UnwrappedTSN::AddTo(cumulative_tsn_ack, block.start));
    auto end = outstanding_data_.upper_bound(
        UnwrappedTSN::AddTo(cumulative_tsn_ack, block.end));
    for (auto iter = start; iter != end; ++iter) {
      AckChunk(ack_info, iter);
    }
  }
}

void OutstandingData::NackBetweenAckBlocks(
    UnwrappedTSN cumulative_tsn_ack,
    rtc::ArrayView<const SackChunk::GapAckBlock> gap_ack_blocks,
    bool is_in_fast_recovery,
    OutstandingData::AckInfo& ack_info) {
  // Mark everything between the blocks as NACKED/TO_BE_RETRANSMITTED.
  // https://tools.ietf.org/html/rfc4960#section-7.2.4
  // "Mark the DATA chunk(s) with three miss indications for retransmission."
  // "For each incoming SACK, miss indications are incremented only for
  // missing TSNs prior to the highest TSN newly acknowledged in the SACK."
  //
  // What this means is that only when there is a increasing stream of data
  // received and there are new packets seen (since last time), packets that are
  // in-flight and between gaps should be nacked. This means that SCTP relies on
  // the T3-RTX-timer to re-send packets otherwise.
  UnwrappedTSN max_tsn_to_nack = ack_info.highest_tsn_acked;
  if (is_in_fast_recovery && cumulative_tsn_ack > last_cumulative_tsn_ack_) {
    // https://tools.ietf.org/html/rfc4960#section-7.2.4
    // "If an endpoint is in Fast Recovery and a SACK arrives that advances
    // the Cumulative TSN Ack Point, the miss indications are incremented for
    // all TSNs reported missing in the SACK."
    max_tsn_to_nack = UnwrappedTSN::AddTo(
        cumulative_tsn_ack,
        gap_ack_blocks.empty() ? 0 : gap_ack_blocks.rbegin()->end);
  }

  UnwrappedTSN prev_block_last_acked = cumulative_tsn_ack;
  for (auto& block : gap_ack_blocks) {
    UnwrappedTSN cur_block_first_acked =
        UnwrappedTSN::AddTo(cumulative_tsn_ack, block.start);
    for (auto iter = outstanding_data_.upper_bound(prev_block_last_acked);
         iter != outstanding_data_.lower_bound(cur_block_first_acked); ++iter) {
      if (iter->first <= max_tsn_to_nack) {
        ack_info.has_packet_loss |=
            NackItem(iter->first, iter->second, /*retransmit_now=*/false,
                     /*do_fast_retransmit=*/!is_in_fast_recovery);
      }
    }
    prev_block_last_acked = UnwrappedTSN::AddTo(cumulative_tsn_ack, block.end);
  }

  // Note that packets are not NACKED which are above the highest gap-ack-block
  // (or above the cumulative ack TSN if no gap-ack-blocks) as only packets
  // up until the highest_tsn_acked (see above) should be considered when
  // NACKing.
}

bool OutstandingData::NackItem(UnwrappedTSN tsn,
                               Item& item,
                               bool retransmit_now,
                               bool do_fast_retransmit) {
  if (item.is_outstanding()) {
    outstanding_bytes_ -= GetSerializedChunkSize(item.data());
    --outstanding_items_;
  }

  switch (item.Nack(retransmit_now)) {
    case Item::NackAction::kNothing:
      return false;
    case Item::NackAction::kRetransmit:
      if (do_fast_retransmit) {
        to_be_fast_retransmitted_.insert(tsn);
      } else {
        to_be_retransmitted_.insert(tsn);
      }
      RTC_DLOG(LS_VERBOSE) << *tsn.Wrap() << " marked for retransmission";
      break;
    case Item::NackAction::kAbandon:
      RTC_DLOG(LS_VERBOSE) << *tsn.Wrap() << " Nacked, resulted in abandoning";
      AbandonAllFor(item);
      break;
  }
  return true;
}

void OutstandingData::AbandonAllFor(const Item& item) {
  // Erase all remaining chunks from the producer, if any.
  if (discard_from_send_queue_(item.data().stream_id, item.message_id())) {
    // There were remaining chunks to be produced for this message. Since the
    // receiver may have already received all chunks (up till now) for this
    // message, we can't just FORWARD-TSN to the last fragment in this
    // (abandoned) message and start sending a new message, as the receiver will
    // then see a new message before the end of the previous one was seen (or
    // skipped over). So create a new fragment, representing the end, that the
    // received will never see as it is abandoned immediately and used as cum
    // TSN in the sent FORWARD-TSN.
    Data message_end(item.data().stream_id, item.data().ssn, item.data().mid,
                     item.data().fsn, item.data().ppid, std::vector<uint8_t>(),
                     Data::IsBeginning(false), Data::IsEnd(true),
                     item.data().is_unordered);
    UnwrappedTSN tsn = next_tsn();
    Item& added_item =
        outstanding_data_
            .emplace(std::piecewise_construct, std::forward_as_tuple(tsn),
                     std::forward_as_tuple(
                         item.message_id(), std::move(message_end),
                         Timestamp::Zero(), MaxRetransmits(0),
                         Timestamp::PlusInfinity(), LifecycleId::NotSet()))
            .first->second;
    // The added chunk shouldn't be included in `outstanding_bytes`, so set it
    // as acked.
    added_item.Ack();
    RTC_DLOG(LS_VERBOSE) << "Adding unsent end placeholder for message at tsn="
                         << *tsn.Wrap();
  }

  for (auto& [tsn, other] : outstanding_data_) {
    if (!other.is_abandoned() &&
        other.data().stream_id == item.data().stream_id &&
        other.message_id() == item.message_id()) {
      RTC_DLOG(LS_VERBOSE) << "Marking chunk " << *tsn.Wrap()
                           << " as abandoned";
      if (other.should_be_retransmitted()) {
        to_be_fast_retransmitted_.erase(tsn);
        to_be_retransmitted_.erase(tsn);
      }
      other.Abandon();
    }
  }
}

std::vector<std::pair<TSN, Data>> OutstandingData::ExtractChunksThatCanFit(
    std::set<UnwrappedTSN>& chunks,
    size_t max_size) {
  std::vector<std::pair<TSN, Data>> result;

  for (auto it = chunks.begin(); it != chunks.end();) {
    UnwrappedTSN tsn = *it;
    auto elem = outstanding_data_.find(tsn);
    RTC_DCHECK(elem != outstanding_data_.end());
    Item& item = elem->second;
    RTC_DCHECK(item.should_be_retransmitted());
    RTC_DCHECK(!item.is_outstanding());
    RTC_DCHECK(!item.is_abandoned());
    RTC_DCHECK(!item.is_acked());

    size_t serialized_size = GetSerializedChunkSize(item.data());
    if (serialized_size <= max_size) {
      item.MarkAsRetransmitted();
      result.emplace_back(tsn.Wrap(), item.data().Clone());
      max_size -= serialized_size;
      outstanding_bytes_ += serialized_size;
      ++outstanding_items_;
      it = chunks.erase(it);
    } else {
      ++it;
    }
    // No point in continuing if the packet is full.
    if (max_size <= data_chunk_header_size_) {
      break;
    }
  }
  return result;
}

std::vector<std::pair<TSN, Data>>
OutstandingData::GetChunksToBeFastRetransmitted(size_t max_size) {
  std::vector<std::pair<TSN, Data>> result =
      ExtractChunksThatCanFit(to_be_fast_retransmitted_, max_size);

  // https://datatracker.ietf.org/doc/html/rfc4960#section-7.2.4
  // "Those TSNs marked for retransmission due to the Fast-Retransmit algorithm
  // that did not fit in the sent datagram carrying K other TSNs are also marked
  // as ineligible for a subsequent Fast Retransmit.  However, as they are
  // marked for retransmission they will be retransmitted later on as soon as
  // cwnd allows."
  if (!to_be_fast_retransmitted_.empty()) {
    to_be_retransmitted_.insert(to_be_fast_retransmitted_.begin(),
                                to_be_fast_retransmitted_.end());
    to_be_fast_retransmitted_.clear();
  }

  RTC_DCHECK(IsConsistent());
  return result;
}

std::vector<std::pair<TSN, Data>> OutstandingData::GetChunksToBeRetransmitted(
    size_t max_size) {
  // Chunks scheduled for fast retransmission must be sent first.
  RTC_DCHECK(to_be_fast_retransmitted_.empty());
  return ExtractChunksThatCanFit(to_be_retransmitted_, max_size);
}

void OutstandingData::ExpireOutstandingChunks(Timestamp now) {
  for (const auto& [tsn, item] : outstanding_data_) {
    // Chunks that are nacked can be expired. Care should be taken not to expire
    // unacked (in-flight) chunks as they might have been received, but the SACK
    // is either delayed or in-flight and may be received later.
    if (item.is_abandoned()) {
      // Already abandoned.
    } else if (item.is_nacked() && item.has_expired(now)) {
      RTC_DLOG(LS_VERBOSE) << "Marking nacked chunk " << *tsn.Wrap()
                           << " and message " << *item.data().mid
                           << " as expired";
      AbandonAllFor(item);
    } else {
      // A non-expired chunk. No need to iterate any further.
      break;
    }
  }
  RTC_DCHECK(IsConsistent());
}

UnwrappedTSN OutstandingData::highest_outstanding_tsn() const {
  return UnwrappedTSN::AddTo(last_cumulative_tsn_ack_,
                             outstanding_data_.size());
}

absl::optional<UnwrappedTSN> OutstandingData::Insert(
    OutgoingMessageId message_id,
    const Data& data,
    Timestamp time_sent,
    MaxRetransmits max_retransmissions,
    Timestamp expires_at,
    LifecycleId lifecycle_id) {
  // All chunks are always padded to be even divisible by 4.
  size_t chunk_size = GetSerializedChunkSize(data);
  outstanding_bytes_ += chunk_size;
  ++outstanding_items_;
  UnwrappedTSN tsn = next_tsn();
  auto it = outstanding_data_
                .emplace(std::piecewise_construct, std::forward_as_tuple(tsn),
                         std::forward_as_tuple(message_id, data.Clone(),
                                               time_sent, max_retransmissions,
                                               expires_at, lifecycle_id))
                .first;

  if (it->second.has_expired(time_sent)) {
    // No need to send it - it was expired when it was in the send
    // queue.
    RTC_DLOG(LS_VERBOSE) << "Marking freshly produced chunk "
                         << *it->first.Wrap() << " and message "
                         << *it->second.data().mid << " as expired";
    AbandonAllFor(it->second);
    RTC_DCHECK(IsConsistent());
    return absl::nullopt;
  }

  RTC_DCHECK(IsConsistent());
  return tsn;
}

void OutstandingData::NackAll() {
  for (auto& [tsn, item] : outstanding_data_) {
    if (!item.is_acked()) {
      NackItem(tsn, item, /*retransmit_now=*/true,
               /*do_fast_retransmit=*/false);
    }
  }
  RTC_DCHECK(IsConsistent());
}

webrtc::TimeDelta OutstandingData::MeasureRTT(Timestamp now,
                                              UnwrappedTSN tsn) const {
  auto it = outstanding_data_.find(tsn);
  if (it != outstanding_data_.end() && !it->second.has_been_retransmitted()) {
    // https://tools.ietf.org/html/rfc4960#section-6.3.1
    // "Karn's algorithm: RTT measurements MUST NOT be made using
    // packets that were retransmitted (and thus for which it is ambiguous
    // whether the reply was for the first instance of the chunk or for a
    // later instance)"
    return now - it->second.time_sent();
  }
  return webrtc::TimeDelta::PlusInfinity();
}

std::vector<std::pair<TSN, OutstandingData::State>>
OutstandingData::GetChunkStatesForTesting() const {
  std::vector<std::pair<TSN, State>> states;
  states.emplace_back(last_cumulative_tsn_ack_.Wrap(), State::kAcked);
  for (const auto& [tsn, item] : outstanding_data_) {
    State state;
    if (item.is_abandoned()) {
      state = State::kAbandoned;
    } else if (item.should_be_retransmitted()) {
      state = State::kToBeRetransmitted;
    } else if (item.is_acked()) {
      state = State::kAcked;
    } else if (item.is_outstanding()) {
      state = State::kInFlight;
    } else {
      state = State::kNacked;
    }

    states.emplace_back(tsn.Wrap(), state);
  }
  return states;
}

bool OutstandingData::ShouldSendForwardTsn() const {
  if (!outstanding_data_.empty()) {
    auto it = outstanding_data_.begin();
    return it->first == last_cumulative_tsn_ack_.next_value() &&
           it->second.is_abandoned();
  }
  return false;
}

ForwardTsnChunk OutstandingData::CreateForwardTsn() const {
  std::map<StreamID, SSN> skipped_per_ordered_stream;
  UnwrappedTSN new_cumulative_ack = last_cumulative_tsn_ack_;

  for (const auto& [tsn, item] : outstanding_data_) {
    if (stream_reset_breakpoint_tsns_.contains(tsn) ||
        (tsn != new_cumulative_ack.next_value()) || !item.is_abandoned()) {
      break;
    }
    new_cumulative_ack = tsn;
    if (!item.data().is_unordered &&
        item.data().ssn > skipped_per_ordered_stream[item.data().stream_id]) {
      skipped_per_ordered_stream[item.data().stream_id] = item.data().ssn;
    }
  }

  std::vector<ForwardTsnChunk::SkippedStream> skipped_streams;
  skipped_streams.reserve(skipped_per_ordered_stream.size());
  for (const auto& [stream_id, ssn] : skipped_per_ordered_stream) {
    skipped_streams.emplace_back(stream_id, ssn);
  }
  return ForwardTsnChunk(new_cumulative_ack.Wrap(), std::move(skipped_streams));
}

IForwardTsnChunk OutstandingData::CreateIForwardTsn() const {
  std::map<std::pair<IsUnordered, StreamID>, MID> skipped_per_stream;
  UnwrappedTSN new_cumulative_ack = last_cumulative_tsn_ack_;

  for (const auto& [tsn, item] : outstanding_data_) {
    if (stream_reset_breakpoint_tsns_.contains(tsn) ||
        (tsn != new_cumulative_ack.next_value()) || !item.is_abandoned()) {
      break;
    }
    new_cumulative_ack = tsn;
    std::pair<IsUnordered, StreamID> stream_id =
        std::make_pair(item.data().is_unordered, item.data().stream_id);

    if (item.data().mid > skipped_per_stream[stream_id]) {
      skipped_per_stream[stream_id] = item.data().mid;
    }
  }

  std::vector<IForwardTsnChunk::SkippedStream> skipped_streams;
  skipped_streams.reserve(skipped_per_stream.size());
  for (const auto& [stream, mid] : skipped_per_stream) {
    skipped_streams.emplace_back(stream.first, stream.second, mid);
  }

  return IForwardTsnChunk(new_cumulative_ack.Wrap(),
                          std::move(skipped_streams));
}

void OutstandingData::ResetSequenceNumbers(UnwrappedTSN last_cumulative_tsn) {
  RTC_DCHECK(outstanding_data_.empty());
  last_cumulative_tsn_ack_ = last_cumulative_tsn;
}

void OutstandingData::BeginResetStreams() {
  stream_reset_breakpoint_tsns_.insert(next_tsn());
}
}  // namespace dcsctp
