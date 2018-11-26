// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#ifndef YB_MASTER_SYS_CATALOG_INTERNAL_H_
#define YB_MASTER_SYS_CATALOG_INTERNAL_H_

#include "yb/gutil/strings/substitute.h"
#include "yb/master/catalog_manager.h"
#include "yb/tserver/tserver.pb.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/pb_util.h"

namespace yb {
namespace master {

class VisitorBase {
 public:
  VisitorBase() {}
  virtual ~VisitorBase() = default;

  virtual int entry_type() const = 0;

  virtual CHECKED_STATUS Visit(Slice id, Slice data) = 0;

 protected:
};

template <class PersistentDataEntryClass>
class Visitor : public VisitorBase {
 public:
  Visitor() {}
  virtual ~Visitor() = default;

  virtual CHECKED_STATUS Visit(Slice id, Slice data) {
    typename PersistentDataEntryClass::data_type metadata;
    RETURN_NOT_OK_PREPEND(
        pb_util::ParseFromArray(&metadata, data.data(), data.size()),
        "Unable to parse metadata field for item id: " + id.ToBuffer());

    return Visit(id.ToBuffer(), metadata);
  }

  int entry_type() const { return PersistentDataEntryClass::type(); }

 protected:
  virtual CHECKED_STATUS Visit(
      const std::string& id, const typename PersistentDataEntryClass::data_type& metadata) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(Visitor);
};

class SysCatalogWriter {
 public:
  SysCatalogWriter(
      const std::string& tablet_id, const Schema& schema_with_ids, int64_t leader_term)
      : schema_with_ids_(schema_with_ids), leader_term_(leader_term) {
    req_.set_tablet_id(tablet_id);
  }

  ~SysCatalogWriter() = default;

  template <class PersistentDataEntryClass>
  Status MutateItem(
      const MetadataCowWrapper<PersistentDataEntryClass>* item,
      const QLWriteRequestPB::QLStmtType& op_type) {
    bool is_write = op_type == QLWriteRequestPB::QL_STMT_INSERT ||
        op_type == QLWriteRequestPB::QL_STMT_UPDATE;

    QLWriteRequestPB* ql_write = req_.add_ql_write_batch();
    ql_write->set_type(op_type);

    faststring metadata_buf;
    if (is_write) {
      if (!pb_util::SerializeToString(item->metadata().dirty().pb, &metadata_buf)) {
        return STATUS(Corruption, strings::Substitute(
            "Unable to serialize SysCatalog entry of type $0 for id $1.",
            PersistentDataEntryClass::type(), item->id()));
      }

      // Add the metadata column.
      QLColumnValuePB* metadata = ql_write->add_column_values();
      RETURN_NOT_OK(SetColumnId(kSysCatalogTableColMetadata, metadata));
      SetBinaryValue(metadata_buf.ToString(), metadata->mutable_expr());
    }
    // Add column type.
    QLExpressionPB* entity_type = ql_write->add_range_column_values();
    SetInt8Value(PersistentDataEntryClass::type(), entity_type);

    // Add column id.
    QLExpressionPB* entity_id = ql_write->add_range_column_values();
    SetBinaryValue(item->id(), entity_id);

    return Status::OK();
  }

  CHECKED_STATUS SetColumnId(const std::string& column_name, QLColumnValuePB* col_pb) {
    size_t column_index = schema_with_ids_.find_column(column_name);
    if (column_index == Schema::kColumnNotFound) {
      return STATUS_SUBSTITUTE(NotFound, "Couldn't find column $0 in the schema", column_name);
    }
    col_pb->set_column_id(schema_with_ids_.column_id(column_index));
    return Status::OK();
  }

  void SetBinaryValue(const std::string& binary_value, QLExpressionPB* expr_pb) {
    expr_pb->mutable_value()->set_binary_value(binary_value);
  }

  void SetInt8Value(const int8_t int8_value, QLExpressionPB* expr_pb) {
    expr_pb->mutable_value()->set_int8_value(int8_value);
  }

  const tserver::WriteRequestPB& req() const {
    return req_;
  }

  int64_t leader_term() const {
    return leader_term_;
  }

 private:
  const Schema& schema_with_ids_;
  tserver::WriteRequestPB req_;
  const int64_t leader_term_;

  DISALLOW_COPY_AND_ASSIGN(SysCatalogWriter);
};

// Template method defintions must go into a header file.
template <class Item>
CHECKED_STATUS SysCatalogTable::AddItem(Item* item, int64_t leader_term) {
  TRACE_EVENT1("master", "SysCatalogTable::Add",
               "table", item->ToString());
  return AddItems(std::vector<Item*>({ item }), leader_term);
}

template <class Item>
CHECKED_STATUS SysCatalogTable::AddItems(const vector<Item*>& items, int64_t leader_term) {
  return MutateItems(items, QLWriteRequestPB::QL_STMT_INSERT, leader_term);
}

template <class Item>
CHECKED_STATUS SysCatalogTable::AddAndUpdateItems(
    const vector<Item*>& added_items,
    const vector<Item*>& updated_items,
    int64_t leader_term) {
  auto w = NewWriter(leader_term);
  for (const auto& item : added_items) {
    RETURN_NOT_OK(w->MutateItem(item, QLWriteRequestPB::QL_STMT_INSERT));
  }
  for (const auto& item : updated_items) {
    RETURN_NOT_OK(w->MutateItem(item, QLWriteRequestPB::QL_STMT_UPDATE));
  }
  return SyncWrite(w.get());
}

template <class Item>
CHECKED_STATUS SysCatalogTable::UpdateItem(Item* item, int64_t leader_term) {
  TRACE_EVENT1("master", "SysCatalogTable::Update",
               "table", item->ToString());
  return UpdateItems(std::vector<Item*>({ item }), leader_term);
}

template <class Item>
CHECKED_STATUS SysCatalogTable::UpdateItems(const vector<Item*>& items, int64_t leader_term) {
  return MutateItems(items, QLWriteRequestPB::QL_STMT_UPDATE, leader_term);
}

template <class Item>
CHECKED_STATUS SysCatalogTable::DeleteItem(Item* item, int64_t leader_term) {
  TRACE_EVENT1("master", "SysCatalogTable::Delete",
               "table", item->ToString());
  return DeleteItems(std::vector<Item*>({ item }), leader_term);
}

template <class Item>
CHECKED_STATUS SysCatalogTable::DeleteItems(const vector<Item*>& items, int64_t leader_term) {
  return MutateItems(items, QLWriteRequestPB::QL_STMT_DELETE, leader_term);
}

template <class Item>
CHECKED_STATUS SysCatalogTable::MutateItems(
    const vector<Item*>& items, const QLWriteRequestPB::QLStmtType& op_type, int64_t leader_term) {
  auto w = NewWriter(leader_term);
  for (const auto& item : items) {
    RETURN_NOT_OK(w->MutateItem(item, op_type));
  }
  return SyncWrite(w.get());
}

std::unique_ptr<SysCatalogWriter> SysCatalogTable::NewWriter(int64_t leader_term) {
  return std::make_unique<SysCatalogWriter>(kSysCatalogTabletId, schema_with_ids_, leader_term);
}

} // namespace master
} // namespace yb

#endif // YB_MASTER_SYS_CATALOG_INTERNAL_H_
