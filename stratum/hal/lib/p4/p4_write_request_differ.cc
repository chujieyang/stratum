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

// This file contains the P4WriteRequestDiffer and P4WriteRequestReporter
// implementations.

#include "stratum/hal/lib/p4/p4_write_request_differ.h"

#include "glog/logging.h"
#include "google/protobuf/generated_message_reflection.h"
#include "stratum/glue/status/status_macros.h"
#include "stratum/lib/macros.h"
#include "stratum/public/lib/error.h"

namespace stratum {
namespace hal {

P4WriteRequestDiffer::P4WriteRequestDiffer(
    const ::p4::v1::WriteRequest& old_request,
    const ::p4::v1::WriteRequest& new_request)
    : old_request_(old_request), new_request_(new_request) {}

::util::Status P4WriteRequestDiffer::Compare(
    ::p4::v1::WriteRequest* delete_request, ::p4::v1::WriteRequest* add_request,
    ::p4::v1::WriteRequest* modify_request,
    ::p4::v1::WriteRequest* unchanged_request) {
  return DoCompare(delete_request, add_request, modify_request,
                   unchanged_request);
}

::util::Status P4WriteRequestDiffer::DoCompare(
    ::p4::v1::WriteRequest* delete_request, ::p4::v1::WriteRequest* add_request,
    ::p4::v1::WriteRequest* modify_request,
    ::p4::v1::WriteRequest* unchanged_request) {
  ::google::google::protobuf::util::MessageDifferencer msg_differencer;
  P4WriteRequestReporter reporter;
  msg_differencer.ReportDifferencesTo(&reporter);
  if (unchanged_request) {
    msg_differencer.set_report_matches(true);
  }

  // The custom comparator treats table_id and all match fields as the
  // key value for comparing updates.
  auto write_desc = ::p4::v1::WriteRequest::default_instance().GetDescriptor();
  P4WriteRequestComparator comparator;
  msg_differencer.TreatAsMapUsingKeyComparator(
      write_desc->FindFieldByName("updates"), &comparator);
  msg_differencer.set_repeated_field_comparison(
      ::google::google::protobuf::util::MessageDifferencer::AS_SET);
  auto update_desc = ::p4::v1::Update::default_instance().GetDescriptor();
  msg_differencer.IgnoreField(update_desc->FindFieldByName("type"));

  if (!msg_differencer.Compare(old_request_, new_request_)) {
    // When differences occur, the output messages are formed from the
    // updates field indices accumulated by the P4WriteRequestReporter.
    if (delete_request) {
      FillOutputFromReporterIndexes(old_request_, reporter.deleted_indexes(),
                                    ::p4::v1::Update::DELETE, delete_request);
    }
    if (add_request) {
      FillOutputFromReporterIndexes(new_request_, reporter.added_indexes(),
                                    ::p4::v1::Update::INSERT, add_request);
    }
    if (modify_request) {
      FillOutputFromReporterIndexes(new_request_, reporter.modified_indexes(),
                                    ::p4::v1::Update::MODIFY, modify_request);
    }
  }

  if (unchanged_request) {
    unchanged_request->Clear();
    const std::set<int>& unchanged_indexes = reporter.unchanged_indexes();
    for (int u = 0; u < old_request_.updates_size(); ++u) {
      if (unchanged_indexes.find(u) != unchanged_indexes.end())
        *(unchanged_request->add_updates()) = old_request_.updates(u);
    }
  }

  return reporter.status();
}

void P4WriteRequestDiffer::FillOutputFromReporterIndexes(
    const ::p4::v1::WriteRequest& source_request,
    const std::vector<int>& reporter_indexes, ::p4::v1::Update::Type type,
    ::p4::v1::WriteRequest* output_request) {
  output_request->Clear();
  for (int i : reporter_indexes) {
    ::p4::v1::Update* update = output_request->add_updates();
    *update = source_request.updates(i);
    update->set_type(type);
  }
}

P4WriteRequestReporter::P4WriteRequestReporter()
    : status_(::util::OkStatus()) {}

// ReportAdded and ReportDeleted are interested only in changes that roll up
// to the first-level repeated Update message nested inside the compared
// P4 WriteRequests.   Details at lower levels of the field_path are not
// processed.
void P4WriteRequestReporter::ReportAdded(
    const ::google::google::protobuf::Message& message1,
    const ::google::google::protobuf::Message& message2,
    const FieldVector& field_path) {
  if (field_path.size() != 1) return;
  VLOG(1) << "ReportAdded " << field_path[0].field->full_name() << " index "
          << field_path[0].index;
  added_indexes_.push_back(field_path[0].index);
}

void P4WriteRequestReporter::ReportDeleted(
    const ::google::google::protobuf::Message& message1,
    const ::google::google::protobuf::Message& message2,
    const FieldVector& field_path) {
  if (field_path.size() != 1) return;
  VLOG(1) << "ReportAdded " << field_path[0].field->full_name() << " index "
          << field_path[0].index;
  deleted_indexes_.push_back(field_path[0].index);
}

void P4WriteRequestReporter::ReportModified(
    const ::google::google::protobuf::Message& message1,
    const ::google::google::protobuf::Message& message2,
    const FieldVector& field_path) {
  if (field_path.size() != 2) return;
  VLOG(1) << "ReportModified " << field_path[0].field->full_name() << " index "
          << field_path[0].new_index;
  modified_indexes_.push_back(field_path[0].new_index);
}

void P4WriteRequestReporter::ReportMoved(
    const ::google::google::protobuf::Message& message1,
    const ::google::google::protobuf::Message& message2,
    const FieldVector& field_path) {
  if (field_path.size() != 1) return;
  VLOG(1) << "ReportMoved " << field_path[0].field->full_name() << " index "
          << field_path[0].index;
  unchanged_indexes_.insert(field_path[0].index);
}

void P4WriteRequestReporter::ReportMatched(
    const ::google::google::protobuf::Message& message1,
    const ::google::google::protobuf::Message& message2,
    const FieldVector& field_path) {
  if (field_path.size() != 1) return;
  VLOG(1) << "ReportMatched " << field_path[0].field->full_name() << " index "
          << field_path[0].index;
  unchanged_indexes_.insert(field_path[0].index);
}

// To match, both messages must:
//  - Have a table_entry.
//  - Table IDs must be equal.
//  - Both entries must have the same match fields.
bool P4WriteRequestComparator::IsMatch(
    const google::protobuf::Message& message1, const google::protobuf::Message& message2,
    const std::vector<google::protobuf::util::MessageDifferencer::SpecificField>&
    parent_fields) const {
  const ::p4::v1::Update& update1 =
      *google::protobuf::internal::DynamicCastToGenerated<const ::p4::v1::Update>(
          &message1);
  const ::p4::v1::Update& update2 =
      *google::protobuf::internal::DynamicCastToGenerated<const ::p4::v1::Update>(
          &message2);
  if (!update1.entity().has_table_entry()) return false;
  if (!update2.entity().has_table_entry()) return false;
  const auto& table_entry1 = update1.entity().table_entry();
  const auto& table_entry2 = update2.entity().table_entry();
  if (table_entry1.table_id() != table_entry2.table_id()) return false;
  if (table_entry1.match_size() != table_entry2.match_size()) return false;

  // Another MessageDifferencer compares the match fields.  The match fields
  // can be in any order, so the comparison treats them as a map with field_id
  // as the key.
  ::google::google::protobuf::util::MessageDifferencer msg_differencer;
  auto entry_desc = ::p4::v1::TableEntry::default_instance().GetDescriptor();
  auto match_desc = ::p4::v1::FieldMatch::default_instance().GetDescriptor();
  auto match_field_desc = entry_desc->FindFieldByName("match");
  msg_differencer.TreatAsMap(match_field_desc,
                             match_desc->FindFieldByName("field_id"));
  std::vector<const ::google::google::protobuf::FieldDescriptor*> compare_fields = {
      match_field_desc};
  return msg_differencer.CompareWithFields(table_entry1, table_entry2,
                                           compare_fields, compare_fields);
}

}  // namespace hal
}  // namespace stratum
