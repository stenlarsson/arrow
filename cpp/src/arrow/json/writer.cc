// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/json/writer.h"

#include "arrow/array.h"
#include "arrow/io/interfaces.h"
#include "arrow/ipc/writer.h"
#include "arrow/json/rapidjson_defs.h"  // IWYU pragma: keep
#include "arrow/record_batch.h"
#include "arrow/result.h"
#include "arrow/type.h"
#include "arrow/type_traits.h"
#include "arrow/util/iterator.h"
#include "arrow/visit_array_inline.h"
#include "arrow/util/checked_cast.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <limits>
#include <memory>
#include <string>

namespace arrow {
namespace json {

namespace rj = arrow::rapidjson;

namespace {

class SingleValueWriter {
public:
  SingleValueWriter(int64_t value_idx, rj::Writer<rj::StringBuffer>& writer, bool emit_null)
      : value_idx_(value_idx), writer_(writer), emit_null_(emit_null) {}

  Status Visit(const arrow::Array& array) {
    return Status::NotImplemented("Single value writing not implemented for type: ",
                                  array.type()->ToString());
  }

  Status Visit(const arrow::BooleanArray& array) {
    writer_.Bool(array.Value(value_idx_));
    return Status::OK();
  }

  template <typename ArrayType, typename DataClass = typename ArrayType::TypeClass>
  arrow::enable_if_t<arrow::has_c_type<DataClass>::value &&
                        !arrow::is_interval_type<DataClass>::value &&
                        !std::is_same<DataClass, arrow::BooleanType>::value,
                    arrow::Status>
  Visit(const ArrayType& array) {
    using CType = typename ArrayType::TypeClass::c_type;
    if constexpr (std::is_floating_point_v<CType>) {
      writer_.Double(static_cast<double>(array.Value(value_idx_)));
    } else {
      writer_.Int64(static_cast<int64_t>(array.Value(value_idx_)));
    }
    return Status::OK();
  }

  template <typename ArrayType, typename T = typename ArrayType::TypeClass>
  arrow::enable_if_string_like<T, arrow::Status> Visit(const ArrayType& array) {
    std::string_view value_view = array.Value(value_idx_);
    writer_.String(value_view.data(), static_cast<rj::SizeType>(value_view.size()));
    return Status::OK();
  }

  Status Visit(const arrow::StructArray& array) {
    const arrow::StructType* struct_type = array.struct_type();
    writer_.StartObject();
    for (int child_idx = 0; child_idx < struct_type->num_fields(); ++child_idx) {
      const arrow::Field* child_field = struct_type->field(child_idx).get();
      const std::string child_field_name = child_field->name();
      const arrow::Array* child_array = array.field(child_idx).get();
      writer_.Key(child_field_name);
      SingleValueWriter child_writer(value_idx_, writer_, emit_null_);
      ARROW_RETURN_NOT_OK(arrow::VisitArrayInline(*child_array, &child_writer));
    }
    writer_.EndObject();
    return Status::OK();
  }

  template <typename ArrayType, typename T = typename ArrayType::TypeClass>
  arrow::enable_if_list_like<T, arrow::Status> Visit(const ArrayType& array) {
    writer_.StartArray();
    auto list_len = array.value_length(value_idx_);
    int64_t list_offset = array.value_offset(value_idx_);
    std::shared_ptr<arrow::Array> list_values = array.values();
    for (int64_t j = 0; j < list_len; ++j) {
      int64_t nested_value_idx = list_offset + j;
      if (list_values->IsNull(nested_value_idx) && !emit_null_) {
        continue;
      }
      SingleValueWriter nested_writer(nested_value_idx, writer_, emit_null_);
      ARROW_RETURN_NOT_OK(arrow::VisitArrayInline(*list_values, &nested_writer));
    }
    writer_.EndArray();
    return Status::OK();
  }

  Status Visit(const arrow::NullArray& array) {
    writer_.Null();
    return Status::OK();
  }

private:
  int64_t value_idx_;
  rj::Writer<rj::StringBuffer>& writer_;
  bool emit_null_;
};

class ArrayWriter {
public:
  ArrayWriter(const std::string& field_name,
                    std::vector<std::unique_ptr<rj::Writer<rj::StringBuffer>>>& writers,
                    bool emit_null,
                    const std::function<bool(int64_t)>& skip_row = nullptr)
      : field_name_(field_name), writers_(writers), emit_null_(emit_null), skip_row_(skip_row) {}

  // Default implementation
  Status Visit(const arrow::Array& array) {
    return Status::NotImplemented("Direct writing not implemented for type: ",
                                  array.type()->ToString());
  }

  // Handle booleans
  Status Visit(const arrow::BooleanArray& array) {
    for (int64_t i = 0; i < array.length(); ++i) {
      if (skip_row_ && skip_row_(i)) {
        continue;
      }
      if (array.IsNull(i) && !emit_null_) {
        continue;
      }
      writers_[i]->Key(field_name_);
      if (array.IsNull(i)) {
        writers_[i]->Null();
      } else {
        writers_[i]->Bool(array.Value(i));
      }
    }
    return Status::OK();
  }

  // Handle primitive numeric types (integers, floats, temporal types)
  template <typename ArrayType, typename DataClass = typename ArrayType::TypeClass>
  arrow::enable_if_t<arrow::has_c_type<DataClass>::value &&
                        !arrow::is_interval_type<DataClass>::value &&
                        !std::is_same<DataClass, arrow::BooleanType>::value,
                    arrow::Status>
  Visit(const ArrayType& array) {
    using CType = typename ArrayType::TypeClass::c_type;

    for (int64_t i = 0; i < array.length(); ++i) {
      if (skip_row_ && skip_row_(i)) {
        continue;
      }
      if (array.IsNull(i) && !emit_null_) {
        continue;
      }
      writers_[i]->Key(field_name_);
      if (array.IsNull(i)) {
        writers_[i]->Null();
      } else {
        if constexpr (std::is_floating_point_v<CType>) {
          writers_[i]->Double(static_cast<double>(array.Value(i)));
        } else {
          writers_[i]->Int64(static_cast<int64_t>(array.Value(i)));
        }
      }
    }
    return Status::OK();
  }

  // Handle string types
  template <typename ArrayType, typename T = typename ArrayType::TypeClass>
  arrow::enable_if_string_like<T, arrow::Status> Visit(const ArrayType& array) {
    for (int64_t i = 0; i < array.length(); ++i) {
      if (skip_row_ && skip_row_(i)) {
        continue;
      }
      if (array.IsNull(i) && !emit_null_) {
        continue;
      }
      writers_[i]->Key(field_name_);
      if (array.IsNull(i)) {
        writers_[i]->Null();
      } else {
        std::string_view value_view = array.Value(i);
        writers_[i]->String(value_view.data(),
                            static_cast<rj::SizeType>(value_view.size()));
      }
    }
    return Status::OK();
  }

  // Handle struct arrays
  Status Visit(const arrow::StructArray& array) {
    const arrow::StructType* type = array.struct_type();

    for (int64_t i = 0; i < array.length(); ++i) {
      if (array.IsNull(i) && !emit_null_) {
        continue;
      }
      writers_[i]->Key(field_name_);
      if (array.IsNull(i)) {
        writers_[i]->Null();
      } else {
        writers_[i]->StartObject();
      }
    }

    // Create a skip function that skips null struct rows
    auto skip_null_struct = [&array, emit_null = emit_null_](int64_t i) -> bool {
      return array.IsNull(i) && !emit_null;
    };

    for (int child_idx = 0; child_idx < type->num_fields(); ++child_idx) {
      const arrow::Field* child_field = type->field(child_idx).get();
      const std::string child_field_name = child_field->name();
      const arrow::Array* child_array = array.field(child_idx).get();

      // Use ArrayWriter with row filter to skip null struct rows
      ArrayWriter child_writer(child_field_name, writers_, emit_null_, skip_null_struct);
      ARROW_RETURN_NOT_OK(arrow::VisitArrayInline(*child_array, &child_writer));
    }

    // End objects for all rows
    for (int64_t i = 0; i < array.length(); ++i) {
      if (array.IsNull(i) && !emit_null_) {
        continue;
      }
      if (!array.IsNull(i)) {
        rj::Writer<rj::StringBuffer>* writer = writers_[i].get();
        writer->EndObject();
      }
    }
    return Status::OK();
  }

  // Handle list-like types
  template <typename ArrayType, typename T = typename ArrayType::TypeClass>
  arrow::enable_if_list_like<T, arrow::Status> Visit(const ArrayType& array) {
    std::shared_ptr<arrow::Array> values = array.values();

    for (int64_t i = 0; i < array.length(); ++i) {
      if (skip_row_ && skip_row_(i)) {
        continue;
      }
      if (array.IsNull(i) && !emit_null_) {
        continue;
      }
      writers_[i]->Key(field_name_);
      if (array.IsNull(i)) {
        writers_[i]->Null();
      } else {
        // Start array
        writers_[i]->StartArray();
        
        // Write all values in the list for this row
        auto array_len = array.value_length(i);
        int64_t offset = array.value_offset(i);
        
        for (int64_t j = 0; j < array_len; ++j) {
          int64_t value_idx = offset + j;
          if (values->IsNull(value_idx) && !emit_null_) {
            continue;
          }
          
          // Write single value to the current row's writer
          ARROW_RETURN_NOT_OK(WriteSingleValue(*values, value_idx, *writers_[i], emit_null_));
        }
        
        // End array
        writers_[i]->EndArray();
      }
    }
    return Status::OK();
  }

  // Handle null arrays
  Status Visit(const arrow::NullArray& array) {
    if (emit_null_) {
      for (int64_t i = 0; i < array.length(); ++i) {
        if (skip_row_ && skip_row_(i)) {
          continue;
        }
        writers_[i]->Key(field_name_);
        writers_[i]->Null();
      }
    }
    return Status::OK();
  }

private:

  // Helper to write a single value from an array to a specific writer
  Status WriteSingleValue(const arrow::Array& values, int64_t value_idx,
                          rj::Writer<rj::StringBuffer>& writer, bool emit_null) {
    if (values.IsNull(value_idx) && !emit_null) {
      return Status::OK();
    }
    
    if (values.IsNull(value_idx)) {
      writer.Null();
      return Status::OK();
    }
    
    // Use a visitor to write the value based on its type
    SingleValueWriter value_writer(value_idx, writer, emit_null);
    return arrow::VisitArrayInline(values, &value_writer);
  }


  const std::string& field_name_;
  std::vector<std::unique_ptr<rj::Writer<rj::StringBuffer>>>& writers_;
  bool emit_null_;
  const std::function<bool(int64_t)>& skip_row_;  // Optional function to skip rows
};
 
class JSONWriterImpl : public ipc::RecordBatchWriter {
 public:
  static Result<std::shared_ptr<JSONWriterImpl>> Make(
      io::OutputStream* sink, std::shared_ptr<io::OutputStream> owned_sink,
      std::shared_ptr<Schema> schema, const WriteOptions& options) {
    RETURN_NOT_OK(options.Validate());
    auto writer = std::make_shared<JSONWriterImpl>(sink, std::move(owned_sink),
                                                   std::move(schema), options);
    return writer;
  }

  Status WriteRecordBatch(const RecordBatch& batch) override {
    const int64_t num_rows = batch.num_rows();
    const int num_columns = batch.num_columns();

    // Create StringBuffer and Writer for each row
    std::vector<std::unique_ptr<rj::StringBuffer>> buffers;
    std::vector<std::unique_ptr<rj::Writer<rj::StringBuffer>>> writers;
    buffers.reserve(num_rows);
    writers.reserve(num_rows);

    for (int64_t i = 0; i < num_rows; ++i) {
      buffers.emplace_back(std::make_unique<rj::StringBuffer>());
      writers.emplace_back(
          std::make_unique<rj::Writer<rj::StringBuffer>>(*buffers[i].get()));
      writers[i]->StartObject();
    }

    // Process each column and write directly to row writers
    for (int col_idx = 0; col_idx < num_columns; ++col_idx) {
      const arrow::Field* field = batch.schema()->field(col_idx).get();
      const std::string field_name = field->name();
      const arrow::Array* array = batch.column(col_idx).get();

      ArrayWriter writer(field_name, writers, options_.emit_null);
      ARROW_RETURN_NOT_OK(arrow::VisitArrayInline(*array, &writer));
    }

    // Finalize objects and write to output
    for (int64_t i = 0; i < num_rows; ++i) {
      writers[i]->EndObject();
      buffers[i]->Put('\n');
      RETURN_NOT_OK(sink_->Write(buffers[i]->GetString()));
    }

    stats_.num_record_batches++;
    return Status::OK();
  }

  Status WriteTable(const Table& table, int64_t max_chunksize) override {
    TableBatchReader reader(table);
    reader.set_chunksize(max_chunksize > 0 ? max_chunksize : options_.batch_size);
    std::shared_ptr<RecordBatch> batch;
    RETURN_NOT_OK(reader.ReadNext(&batch));
    while (batch != nullptr) {
      RETURN_NOT_OK(WriteRecordBatch(*batch));
      RETURN_NOT_OK(reader.ReadNext(&batch));
    }
    return Status::OK();
  }

  Status Close() override { return Status::OK(); }

  ipc::WriteStats stats() const override { return stats_; }

  JSONWriterImpl(io::OutputStream* sink, std::shared_ptr<io::OutputStream> owned_sink,
                 std::shared_ptr<Schema> schema, const WriteOptions& options)
      : sink_(sink),
        owned_sink_(std::move(owned_sink)),
        schema_(std::move(schema)),
        options_(options) {}

 private:
  io::OutputStream* sink_;
  std::shared_ptr<io::OutputStream> owned_sink_;
  const std::shared_ptr<Schema> schema_;
  const WriteOptions options_;
  ipc::WriteStats stats_;
};

}  // namespace

Status WriteJSON(const Table& table, const WriteOptions& options,
                 arrow::io::OutputStream* output) {
  ARROW_ASSIGN_OR_RAISE(auto writer, MakeJSONWriter(output, table.schema(), options));
  RETURN_NOT_OK(writer->WriteTable(table));
  return writer->Close();
}

Status WriteJSON(const RecordBatch& batch, const WriteOptions& options,
                 arrow::io::OutputStream* output) {
  ARROW_ASSIGN_OR_RAISE(auto writer, MakeJSONWriter(output, batch.schema(), options));
  RETURN_NOT_OK(writer->WriteRecordBatch(batch));
  return writer->Close();
}

Status WriteJSON(const std::shared_ptr<RecordBatchReader>& reader,
                 const WriteOptions& options, arrow::io::OutputStream* output) {
  ARROW_ASSIGN_OR_RAISE(auto writer, MakeJSONWriter(output, reader->schema(), options));
  std::shared_ptr<RecordBatch> batch;
  while (true) {
    ARROW_ASSIGN_OR_RAISE(batch, reader->Next());
    if (batch == nullptr) break;
    RETURN_NOT_OK(writer->WriteRecordBatch(*batch));
  }
  return writer->Close();
}

ARROW_EXPORT
Result<std::shared_ptr<ipc::RecordBatchWriter>> MakeJSONWriter(
    std::shared_ptr<io::OutputStream> sink, const std::shared_ptr<Schema>& schema,
    const WriteOptions& options) {
  return JSONWriterImpl::Make(sink.get(), sink, schema, options);
}

ARROW_EXPORT
Result<std::shared_ptr<ipc::RecordBatchWriter>> MakeJSONWriter(
    io::OutputStream* sink, const std::shared_ptr<Schema>& schema,
    const WriteOptions& options) {
  return JSONWriterImpl::Make(sink, nullptr, schema, options);
}

}  // namespace json
}  // namespace arrow
