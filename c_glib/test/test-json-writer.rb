# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

class TestJSONWriter < Test::Unit::TestCase
  include Helper::Buildable

  def test_write_record_batch
    message_data = ["Start", "Shutdown"]
    count_data = [2, 9]
    message_field = Arrow::Field.new("message", Arrow::StringDataType.new)
    count_field = Arrow::Field.new("count", Arrow::Int64DataType.new)
    schema = Arrow::Schema.new([message_field, count_field])

    buffer = Arrow::ResizableBuffer.new(0)
    output = Arrow::BufferOutputStream.new(buffer)
    begin
      json_writer = Arrow::JSONWriter.new(output, schema)
      begin
        record_batch = Arrow::RecordBatch.new(schema,
                                              message_data.size,
                                              [
                                                build_string_array(message_data),
                                                build_int64_array(count_data),
                                              ])
        json_writer.write_record_batch(record_batch)
      ensure
        json_writer.close
        assert do
          json_writer.closed?
        end
      end
    ensure
      output.close
    end

    json_output = buffer.data.to_s
    expected = <<~JSON
      {"message":"Start","count":2}
      {"message":"Shutdown","count":9}
    JSON
    assert_equal(expected, json_output)
  end

  def test_write_table
    message_data = ["Start", "Shutdown", "Reboot"]
    count_data = [2, 9, 5]
    message_field = Arrow::Field.new("message", Arrow::StringDataType.new)
    count_field = Arrow::Field.new("count", Arrow::Int64DataType.new)
    schema = Arrow::Schema.new([message_field, count_field])

    buffer = Arrow::ResizableBuffer.new(0)
    output = Arrow::BufferOutputStream.new(buffer)
    begin
      json_writer = Arrow::JSONWriter.new(output, schema)
      begin
        table = Arrow::Table.new(schema,
                                 [
                                   build_string_array(message_data),
                                   build_int64_array(count_data),
                                 ])
        json_writer.write_table(table)
      ensure
        json_writer.close
        assert do
          json_writer.closed?
        end
      end
    ensure
      output.close
    end

    json_output = buffer.data.to_s
    expected = <<~JSON
      {"message":"Start","count":2}
      {"message":"Shutdown","count":9}
      {"message":"Reboot","count":5}
    JSON
    assert_equal(expected, json_output)
  end


  sub_test_case("options") do
    def setup
      @options = Arrow::JSONWriteOptions.new
    end

    def test_batch_size
      assert_equal(1024, @options.batch_size)
      @options.batch_size = 2048
      assert_equal(2048, @options.batch_size)
    end

    def test_emit_null
      assert do
        not @options.emit_null?
      end
      @options.emit_null = true
      assert do
        @options.emit_null?
      end
    end

    def test_write_with_options_emit_null
      message_data = ["Start", nil, "Reboot"]
      count_data = [2, 9, 5]
      message_field = Arrow::Field.new("message", Arrow::StringDataType.new)
      count_field = Arrow::Field.new("count", Arrow::Int64DataType.new)
      schema = Arrow::Schema.new([message_field, count_field])

      options = Arrow::JSONWriteOptions.new
      options.emit_null = true

      buffer = Arrow::ResizableBuffer.new(0)
      output = Arrow::BufferOutputStream.new(buffer)
      begin
        json_writer = Arrow::JSONWriter.new(output, schema, options)
        begin
          record_batch = Arrow::RecordBatch.new(schema,
                                                message_data.size,
                                                [
                                                  build_string_array(message_data),
                                                  build_int64_array(count_data),
                                                ])
          json_writer.write_record_batch(record_batch)
        ensure
          json_writer.close
        end
      ensure
        output.close
      end

      json_output = buffer.data.to_s
      expected = <<~JSON
        {"message":"Start","count":2}
        {"message":null,"count":9}
        {"message":"Reboot","count":5}
      JSON
      assert_equal(expected, json_output)
    end

    def test_write_with_options_skip_null
      message_data = ["Start", nil, "Reboot"]
      count_data = [2, 9, 5]
      message_field = Arrow::Field.new("message", Arrow::StringDataType.new)
      count_field = Arrow::Field.new("count", Arrow::Int64DataType.new)
      schema = Arrow::Schema.new([message_field, count_field])

      options = Arrow::JSONWriteOptions.new
      options.emit_null = false

      buffer = Arrow::ResizableBuffer.new(0)
      output = Arrow::BufferOutputStream.new(buffer)
      begin
        json_writer = Arrow::JSONWriter.new(output, schema, options)
        begin
          record_batch = Arrow::RecordBatch.new(schema,
                                                message_data.size,
                                                [
                                                  build_string_array(message_data),
                                                  build_int64_array(count_data),
                                                ])
          json_writer.write_record_batch(record_batch)
        ensure
          json_writer.close
        end
      ensure
        output.close
      end

      json_output = buffer.data.to_s
      expected = <<~JSON
        {"message":"Start","count":2}
        {"count":9}
        {"message":"Reboot","count":5}
      JSON
      assert_equal(expected, json_output)
    end
  end
end

