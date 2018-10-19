// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#include "stratum/hal/lib/phal/attribute_database.h"

#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "google/protobuf/util/message_differencer.h"
#include "stratum/glue/status/status_macros.h"
#include "stratum/hal/lib/phal/dummy_threadpool.h"
//#include "stratum/hal/lib/phal/google_platform/google_switch_configurator.h"
#include "stratum/lib/macros.h"
#include "stratum/lib/utils.h"
#include "absl/memory/memory.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

namespace stratum {
namespace hal {
namespace phal {

DatabaseQuery::DatabaseQuery(AttributeDatabase* database,
                             AttributeGroup* root_group,
                             ThreadpoolInterface* threadpool)
    : database_(database),
      query_(root_group, threadpool),
      last_polling_time_(absl::InfinitePast()) {
  absl::MutexLock lock(&database_->polling_lock_);
  database->polling_queries_.insert(this);
}

DatabaseQuery::~DatabaseQuery() {
  absl::MutexLock lock(&database_->polling_lock_);
  database_->polling_queries_.erase(this);
}

::util::StatusOr<std::unique_ptr<PhalDB>> DatabaseQuery::Get() {
  auto query_result = absl::make_unique<PhalDB>();
  RETURN_IF_ERROR(query_.Get(query_result.get()));
  return std::move(query_result);
}

::util::Status DatabaseQuery::Poll(absl::Time poll_time) {
  // Update the polling time first. Otherwise if a query starts failing
  // repeatedly we'll just busy loop on it forever.
  last_polling_time_ = poll_time;
  // If the query is already marked as updated (e.g. due to a runtime
  // configurator), it's a waste of time to check for updates.
  if (!query_.IsUpdated()) {
    // If the result of this query has changed, set the update bit.
    ASSIGN_OR_RETURN(auto polling_result, Get());
    if (last_polling_result_ == nullptr ||
        !google::protobuf::util::MessageDifferencer::Equals(*last_polling_result_,
                                                    *polling_result)) {
      query_.MarkUpdated();
      last_polling_result_ = std::move(polling_result);
    }
  }
  return ::util::OkStatus();
}

// Note: We assume that there will rarely be multiple subscribers on a single
// query, so we keep multi-subscriber support very simple. If two subscribers
// are added to the same query, they will both be updated at the shorter of
// their polling intervals.
::util::Status DatabaseQuery::Subscribe(
    std::unique_ptr<ChannelWriter<PhalDB>> subscriber,
    absl::Duration polling_interval) {
  absl::MutexLock lock(&database_->polling_lock_);
  subscribers_.emplace_back(std::move(subscriber), polling_interval);
  // Send an initial message to the new subscriber. We'll also incidentally send
  // messages to all existing subscribers.
  query_.MarkUpdated();
  // The polling interval for this query may be different due to the new query.
  RecalculatePollingInterval();
  // Wake up the polling thread to respond to this new subscriber.
  database_->polling_condvar_.Signal();
  return ::util::OkStatus();
}

void DatabaseQuery::RecalculatePollingInterval() {
  // This uses a naive linear algorithm rather than anything more fancy because
  // we're unlikely to every have more than 2 or 3 subscribers on a single
  // query.
  polling_interval_ = absl::InfiniteDuration();
  for (const auto& subscriber : subscribers_) {
    const absl::Duration& subscriber_interval = subscriber.second;
    if (subscriber_interval < polling_interval_)
      polling_interval_ = subscriber_interval;
  }
}

::util::Status DatabaseQuery::UpdateSubscribers() {
  ASSIGN_OR_RETURN(auto polling_result, Get());
  bool subscribers_removed = false;
  for (int i = 0; i < subscribers_.size(); i++) {
    ChannelWriter<PhalDB>* channel = subscribers_[i].first.get();
    ::util::Status write_result = channel->TryWrite(*polling_result);
    if (!write_result.ok()) {
      // This failure may be due to the channel closing, which is the expected
      // unsubscribe mechanism. Otherwise, this is considered an error.
      if (channel->IsClosed()) {
        subscribers_.erase(subscribers_.begin() + i);
        i--;
        subscribers_removed = true;
      } else {
        return APPEND_ERROR(write_result) << " Failed to update subscribers.";
      }
    }
  }
  if (subscribers_removed) RecalculatePollingInterval();
  query_.ClearUpdated();
  last_polling_result_ = std::move(polling_result);
  return ::util::OkStatus();
}

absl::Time DatabaseQuery::GetNextPollingTime() {
  // Handle the special case where we have infinite-past + infinite-duration.
  if (polling_interval_ == absl::InfiniteDuration())
    return absl::InfiniteFuture();
  return last_polling_time_ + polling_interval_;
}

::util::StatusOr<std::unique_ptr<AttributeDatabase>> AttributeDatabase::Make(
    std::unique_ptr<AttributeGroup> root,
    std::unique_ptr<ThreadpoolInterface> threadpool, bool run_polling_thread) {
  CHECK_RETURN_IF_FALSE(root->AcquireReadable()->GetDescriptor() ==
                        PhalDB::descriptor())
      << "The root group of a AttributeDatabase must use "
      << "PhalDB as its schema.";
  std::unique_ptr<AttributeDatabase> database = absl::WrapUnique(
      new AttributeDatabase(std::move(root), std::move(threadpool)));
  if (run_polling_thread)
    RETURN_IF_ERROR(database->SetupPolling());
  return std::move(database);
}

AttributeDatabase::~AttributeDatabase() {
  TeardownPolling();
  // We delete the database first, since we might otherwise make broken calls
  // into the configurator.
  root_ = nullptr;
}

::util::StatusOr<std::unique_ptr<AttributeDatabase>>
AttributeDatabase::MakeGoogle(const std::string& legacy_phal_config_path,
                              const SystemInterface* system_interface) {
  LegacyPhalInitConfig config;
  RETURN_IF_ERROR(ReadProtoFromTextFile(legacy_phal_config_path, &config));
  std::unique_ptr<AttributeGroup> root_group =
      AttributeGroup::From(PhalDB::descriptor());
  ASSIGN_OR_RETURN(std::unique_ptr<UdevEventHandler> udev,
                   UdevEventHandler::MakeUdevEventHandler(system_interface));
  auto configurator =
      absl::make_unique<GoogleSwitchConfigurator>(system_interface, udev.get());
  RETURN_IF_ERROR(configurator->ConfigureSwitch(config, root_group.get()));
  ASSIGN_OR_RETURN(
      std::unique_ptr<AttributeDatabase> database,
      Make(std::move(root_group), absl::make_unique<DummyThreadpool>()));
  AttributeDatabase* database_ptr = database.get();
  udev->AddUpdateCallback([database_ptr](::util::Status update_status) -> void {
    if (update_status.ok()) {
      absl::MutexLock lock(&database_ptr->polling_lock_);
      ::util::Status result = database_ptr->FlushQueries();
      if (!result.ok()) {
        LOG(ERROR) << "Failed to send a streaming query update after a udev "
                   << "event with status " << result;
      }
    }
  });
  database->udev_ = std::move(udev);
  database->google_switch_configurator_ = std::move(configurator);
  return std::move(database);
}

::util::StatusOr<std::unique_ptr<AttributeDatabase>>
AttributeDatabase::MakeOnlp() {
  return MAKE_ERROR() << "AttributeDatabase::MakeOnlp is not yet implemented.";
}

::util::Status AttributeDatabase::Set(const AttributeValueMap& values) {
  absl::MutexLock lock(&set_lock_);
  return root_->Set(values, threadpool_.get());
}

::util::StatusOr<std::unique_ptr<Query>> AttributeDatabase::MakeQuery(
    const std::vector<Path>& query_paths) {
  auto query =
      absl::WrapUnique(new DatabaseQuery(this, root_.get(), threadpool_.get()));
  RETURN_IF_ERROR(root_->AcquireReadable()->RegisterQuery(
      query->InternalQuery(), query_paths));
  // An implicit cast doesn't work here.
  return absl::WrapUnique<Query>(query.release());
}

::util::Status AttributeDatabase::SetupPolling() {
  absl::MutexLock lock(&polling_lock_);
  CHECK_RETURN_IF_FALSE(!polling_thread_running_) <<
      "Called SetupPolling(), but the polling thread is already running!";
  polling_thread_running_ = true;
  if (pthread_create(&polling_thread_id_, nullptr,
                      &AttributeDatabase::RunPollingThread, this)) {
    polling_thread_running_ = false;
    return MAKE_ERROR()
           << "Failed to initalize the AttributeDatabase polling thread.";
  }
  return ::util::OkStatus();
}

void AttributeDatabase::TeardownPolling() {
  bool running = false;
  {
    absl::MutexLock lock(&polling_lock_);
    std::swap(running, polling_thread_running_);
    if (!polling_queries_.empty()) {
      LOG(ERROR)
          << "Called TeardownPolling while polling queries are still running.";
    }
    // At this point PollQueries() should be blocking indefinitely, so we need
    // to wake it up.
    polling_condvar_.Signal();
  }
  if (running) pthread_join(polling_thread_id_, nullptr);
}

void* AttributeDatabase::RunPollingThread(void* attribute_database_ptr) {
  AttributeDatabase* attribute_database =
      static_cast<AttributeDatabase*>(attribute_database_ptr);
  absl::MutexLock lock(&attribute_database->polling_lock_);
  while (attribute_database->polling_thread_running_) {
    // Wait until we need to poll some query or some other event has indicated
    // that there are messages to be sent (e.g. a new subscriber).
    attribute_database->polling_condvar_.WaitWithDeadline(
        &attribute_database->polling_lock_,
        attribute_database->GetNextPollingTime());
    // Exit early if the CondVar was signalled due to the AttributeDatabase
    // shutting down.
    if (!attribute_database->polling_thread_running_) break;

    ::util::Status result = attribute_database->PollQueries();
    if (!result.ok()) {
      LOG(ERROR) << "Failed to poll a streaming query with status " << result;
    }
    result = attribute_database->FlushQueries();
    if (!result.ok()) {
      LOG(ERROR) << "Failed to send a streaming query update with status "
                 << result;
    }
  }
  return nullptr;
}

absl::Time AttributeDatabase::GetNextPollingTime() {
  // TODO(swiggett): Use a priority queue here if we end up with lots of
  // streaming queries.
  absl::Time next_poll = absl::InfiniteFuture();
  for (auto query : polling_queries_) {
    absl::Time query_poll = query->GetNextPollingTime();
    if (query_poll < next_poll) next_poll = query_poll;
  }
  return next_poll;
}

::util::Status AttributeDatabase::PollQueries() {
  // Only poll a query if it's polling interval has elapsed.
  absl::Time poll_time = absl::Now();
  for (auto query : polling_queries_) {
    if (query->GetNextPollingTime() <= poll_time) {
      RETURN_IF_ERROR(query->Poll(poll_time));
    }
  }
  return ::util::OkStatus();
}

::util::Status AttributeDatabase::FlushQueries() {
  // We may need to send a message now. Check for updated queries.
  ::util::Status flush_result = ::util::OkStatus();
  for (auto query : polling_queries_) {
    if (query->InternalQuery()->IsUpdated()) {
      APPEND_STATUS_IF_ERROR(flush_result, query->UpdateSubscribers());
    }
  }
  return flush_result;
}

}  // namespace phal
}  // namespace hal
}  // namespace stratum
