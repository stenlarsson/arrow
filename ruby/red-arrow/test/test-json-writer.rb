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

class JSONWriterTest < Test::Unit::TestCase
  def test_write_table
    table = Arrow::Table.new({
      message: ["Start", nil, "Reboot"],
      count: [2, 9, 5],
    })

    buffer = Arrow::ResizableBuffer.new(0)
    Arrow::BufferOutputStream.open(buffer) do |output|
      Arrow::JSONWriter.open(output, table.schema, emit_null: true) do |json_writer|
        json_writer.write_table(table)
      end
    end

    json_output = buffer.data.to_s
    expected = <<~JSON
      {"message":"Start","count":2}
      {"message":null,"count":9}
      {"message":"Reboot","count":5}
    JSON
    assert_equal(expected, json_output)
  end
end

