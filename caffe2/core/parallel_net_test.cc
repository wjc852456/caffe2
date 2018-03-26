/**
 * Copyright (c) 2016-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <chrono>  // NOLINT
#include <ctime>
#include <thread>  // NOLINT

#include "caffe2/core/net.h"
#include "caffe2/core/operator.h"
#include <gtest/gtest.h>

namespace caffe2 {

using std::clock_t;
using std::clock;

// When measuring time, we relax the measured time by +- 20ms.
const int kTimeThreshold = 20;

// SleepOp basically sleeps for a given number of seconds.
// We allow arbitrary inputs and at most one output so that we can
// test scaffolding of networks. If the output is 1, it will be filled with
// vector<clock_t> with two elements: start time and end time.
class SleepOp final : public Operator<CPUContext> {
 public:
  SleepOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator<CPUContext>(operator_def, ws),
        ms_(OperatorBase::GetSingleArgument<int>("ms", 1000)) {
    DCHECK_GT(ms_, 0);
    DCHECK_LT(ms_, 3600 * 1000) << "Really? This long?";
  }

  bool RunOnDevice() override {
    clock_t start = clock();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms_));
    clock_t end = clock();
    if (OperatorBase::OutputSize()) {
      vector<clock_t>* output = OperatorBase::Output<vector<clock_t> >(0);
      output->resize(2);
      (*output)[0] = start;
      (*output)[1] = end;
    }
    return true;
  }

 private:
  int ms_;
};

OPERATOR_SCHEMA(Sleep).NumInputs(0, INT_MAX).NumOutputs(0, 1);

REGISTER_CPU_OPERATOR(Sleep, SleepOp);
REGISTER_CUDA_OPERATOR(Sleep, SleepOp);

const char kSleepNetDefString[] =
"  name: \"sleepnet\""
"  type: \"dag\""
"  num_workers: 2"
"  op {"
"    output: \"sleep1\""
"    name: \"sleep1\""
"    type: \"Sleep\""
"    arg {"
"      name: \"ms\""
"      i: 100"
"    }"
"  }"
"  op {"
"    input: \"sleep1\""
"    output: \"sleep2\""
"    name: \"sleep2\""
"    type: \"Sleep\""
"    arg {"
"      name: \"ms\""
"      i: 100"
"    }"
"  }"
"  op {"
"    output: \"sleep3\""
"    name: \"sleep3\""
"    type: \"Sleep\""
"    arg {"
"      name: \"ms\""
"      i: 150"
"    }"
"  }";

namespace {
// Run a network and get its duration in milliseconds.
int RunNetAndGetDuration(const string& net_def_str, const string& type) {
  NetDef net_def;
  CAFFE_ENFORCE(
      TextFormat::ParseFromString(net_def_str, &net_def));
  net_def.set_type(type);
  Workspace ws;
  unique_ptr<NetBase> net(CreateNet(net_def, &ws));
  CAFFE_ENFORCE(net.get() != nullptr);
  auto start_time = std::chrono::system_clock::now();
  CAFFE_ENFORCE(net->Run());
  // Inspect the time - it should be around 200 milliseconds, since sleep3 can
  // run in parallel with sleep1 and sleep2.
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now() - start_time);
  int milliseconds = duration.count();
  return milliseconds;
}
}  // namespace

TEST(DAGNetTest, TestDAGNetTiming) {
  int ms = RunNetAndGetDuration(string(kSleepNetDefString), "dag");
  EXPECT_NEAR(ms, 200, kTimeThreshold);
}

// For sanity check, we also test the sequential time - it should take 0.35
// seconds instead since everything has to be sequential.
TEST(SimpleNetTest, TestSimpleNetTiming) {
  int ms = RunNetAndGetDuration(string(kSleepNetDefString), "simple");
  EXPECT_NEAR(ms, 350, kTimeThreshold);
}

// This network has two operators reading the same blob at the same time. This
// should not change anything and the DAG should still make sleep2 and sleep3
// run in parallel.
const char kSleepNetDefStringReadAfterRead[] =
"  name: \"sleepnet\""
"  type: \"dag\""
"  num_workers: 2"
"  op {"
"    output: \"sleep1\""
"    name: \"sleep1\""
"    type: \"Sleep\""
"    arg {"
"      name: \"ms\""
"      i: 100"
"    }"
"  }"
"  op {"
"    input: \"sleep1\""
"    output: \"sleep2\""
"    name: \"sleep2\""
"    type: \"Sleep\""
"    arg {"
"      name: \"ms\""
"      i: 100"
"    }"
"  }"
"  op {"
"    input: \"sleep1\""
"    output: \"sleep3\""
"    name: \"sleep3\""
"    type: \"Sleep\""
"    arg {"
"      name: \"ms\""
"      i: 150"
"    }"
"  }";

TEST(DAGNetTest, TestDAGNetTimingReadAfterRead) {
  int ms = RunNetAndGetDuration(string(kSleepNetDefStringReadAfterRead), "dag");
  EXPECT_NEAR(ms, 250, kTimeThreshold);
}

// For sanity check, we also test the sequential time - it should take 0.35
// seconds instead since everything has to be sequential.
TEST(SimpleNetTest, TestSimpleNetTimingReadAfterRead) {
  int ms = RunNetAndGetDuration(string(kSleepNetDefStringReadAfterRead), "simple");
  EXPECT_NEAR(ms, 350, kTimeThreshold);
}

// This network has two operators writing out the sleep2 blob. As a result, the
// operator sleep2-again creates a write after write dependency and the whole
// process should be sequential.
const char kSleepNetDefStringWriteAfterWrite[] =
"  name: \"sleepnet\""
"  type: \"dag\""
"  num_workers: 2"
"  op {"
"    output: \"sleep1\""
"    name: \"sleep1\""
"    type: \"Sleep\""
"    arg {"
"      name: \"ms\""
"      i: 100"
"    }"
"  }"
"  op {"
"    input: \"sleep1\""
"    output: \"sleep2\""
"    name: \"sleep2\""
"    type: \"Sleep\""
"    arg {"
"      name: \"ms\""
"      i: 100"
"    }"
"  }"
"  op {"
"    output: \"sleep2\""
"    name: \"sleep2-again\""
"    type: \"Sleep\""
"    arg {"
"      name: \"ms\""
"      i: 150"
"    }"
"  }";

TEST(DAGNetTest, TestDAGNetTimingWriteAfterWrite) {
  int ms = RunNetAndGetDuration(
      string(kSleepNetDefStringWriteAfterWrite), "dag");
  EXPECT_NEAR(ms, 350, kTimeThreshold);
}

TEST(SimpleNetTest, TestSimpleNetTimingWriteAfterWrite) {
  int ms = RunNetAndGetDuration(
      string(kSleepNetDefStringWriteAfterWrite), "simple");
  EXPECT_NEAR(ms, 350, kTimeThreshold);
}

// This network has an operator writing to sleep1 while another operator is
// accessing it. As a result, the operator sleep1-again creates a write after
// read dependency and the whole process should be sequential.
const char kSleepNetDefStringWriteAfterRead[] =
"  name: \"sleepnet\""
"  type: \"dag\""
"  num_workers: 2"
"  op {"
"    output: \"sleep1\""
"    name: \"sleep1\""
"    type: \"Sleep\""
"    arg {"
"      name: \"ms\""
"      i: 100"
"    }"
"  }"
"  op {"
"    input: \"sleep1\""
"    output: \"sleep2\""
"    name: \"sleep2\""
"    type: \"Sleep\""
"    arg {"
"      name: \"ms\""
"      i: 100"
"    }"
"  }"
"  op {"
"    output: \"sleep1\""
"    name: \"sleep1-again\""
"    type: \"Sleep\""
"    arg {"
"      name: \"ms\""
"      i: 150"
"    }"
"  }";

TEST(DAGNetTest, TestDAGNetTimingWriteAfterRead) {
  int ms = RunNetAndGetDuration(
      string(kSleepNetDefStringWriteAfterRead), "dag");
  EXPECT_NEAR(ms, 350, kTimeThreshold);
}

TEST(SimpleNetTest, TestSimpleNetTimingWriteAfterRead) {
  int ms = RunNetAndGetDuration(
      string(kSleepNetDefStringWriteAfterRead), "simple");
  EXPECT_NEAR(ms, 350, kTimeThreshold);
}

// This network has an operator writing to sleep1 while another
// operator has a control dependency on it. As a result, the operator
// sleep1-again creates a write after read dependency and the whole
// process should be sequential.
const char kSleepNetDefStringControlDependency[] = R"DOC(
  name: "sleepnet"
  type: "dag"
  num_workers: 2
  op {
    output: "sleep1"
    name: "sleep1"
    type: "Sleep"
    arg {
      name: "ms"
      i: 100
    }
  }
  op {
    control_input: "sleep1"
    output: "sleep2"
    name: "sleep2"
    type: "Sleep"
    arg {
      name: "ms"
      i: 100
    }
  }
  op {
    output: "sleep1"
    name: "sleep1-again"
    type: "Sleep"
    arg {
      name: "ms"
      i: 150
    }
  }
)DOC";

TEST(DAGNetTest, TestDAGNetTimingControlDependency) {
  int ms = RunNetAndGetDuration(
      string(kSleepNetDefStringControlDependency), "dag");
  EXPECT_NEAR(ms, 350, kTimeThreshold);
}

TEST(SimpleNetTest, TestSimpleNetTimingControlDependency) {
  int ms = RunNetAndGetDuration(
      string(kSleepNetDefStringControlDependency), "simple");
  EXPECT_NEAR(ms, 350, kTimeThreshold);
}

}  // namespace caffe2