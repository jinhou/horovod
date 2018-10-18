// Copyright 2018 Uber Technologies, Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include "parameter_manager.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "mpi.h"

namespace horovod {
namespace common {

#define WARMUPS 3

#define INVPHI 0.61803398875
#define INVPHI2 0.38196601125
#define TOL 10.0

std::vector<double> CycleTimes() {
  std::vector<double> results;
  for (int i = 0; i < 60; i++) {
    results.push_back(i * 5);
  }
  std::random_shuffle(results.begin(), results.end());
  return results;
}

// ParameterManager
ParameterManager::ParameterManager() :
    hierarchical_allreduce_(CategoricalParameter<bool>(std::vector<bool>{false, true}, *this, nullptr)),
    joint_params_(BayesianParameter(std::vector<std::pair<double, double>>{
      std::pair<double, double>(0, 64), std::pair<double, double>(0, 100)
    }, *this, nullptr)),
//    tensor_fusion_threshold_mb_(CategoricalParameter<int64_t>(
//        std::vector<int64_t>{0, 1, 2, 4, 8, 16, 32, 64}, *this, nullptr)),
//    tensor_fusion_threshold_mb_(NumericParameter<int64_t>(
//        1024 * 1024, 256 * 1024 * 1024, *this, nullptr)),
//    cycle_time_ms_(CategoricalParameter<double>(
//        CycleTimes(), *this, &tensor_fusion_threshold_mb_)),
//    cycle_time_ms_(NumericParameter<double>(1.0, 200.0, *this, &tensor_fusion_threshold_mb_)),
    leaf_param_(&joint_params_),
    active_(false),
    warmup_remaining_(WARMUPS),
    cycle_(0),
    rank_(-1),
    root_rank_(0),
    writing_(false) {
  ReadyTune();
}

void ParameterManager::CreateMpiTypes() {
  const int nitems = 3;
  int blocklengths[3] = {1, 1, 1};
  MPI_Datatype types[3] = {MPI_CXX_BOOL, MPI_DOUBLE, MPI_DOUBLE};

  MPI_Aint offsets[3];
  offsets[0] = offsetof(Params, hierarchical_allreduce);
  offsets[1] = offsetof(Params, tensor_fusion_threshold);
  offsets[2] = offsetof(Params, cycle_time);

  MPI_Type_create_struct(nitems, blocklengths, offsets, types, &mpi_params_type_);
  MPI_Type_commit(&mpi_params_type_);
}

void ParameterManager::FreeMpiTypes() {
  if (mpi_params_type_ != MPI_DATATYPE_NULL) {
    MPI_Type_free(&mpi_params_type_);
  }
}

void ParameterManager::Initialize(int32_t rank, int32_t root_rank, MPI_Comm mpi_comm, std::string file_name) {
  rank_ = rank;
  root_rank_ = root_rank;
  mpi_comm_ = mpi_comm;
  if (rank_ == root_rank && !file_name.empty()) {
    file_.open(file_name, std::ios::out | std::ios::trunc);
    if (file_.good()) {
      file_ << "cycle_time_ms,tensor_fusion_threshold,score" << std::endl;
      writing_ = true;
    }
  }
}

void ParameterManager::SetAutoTuning(bool active) {
  if (active != active_) {
    warmup_remaining_ = WARMUPS;
  }
  active_ = active;
  if (!active_ && rank_ == root_rank_) {
    std::cout << "finished autotuning" << std::endl;
  }
};

bool ParameterManager::HierarchicalAllreduce() const {
  return active_ ? hierarchical_allreduce_.Value() : hierarchical_allreduce_.BestValue();
}

void ParameterManager::SetHierarchicalAllreduce(bool value, bool fixed) {
  hierarchical_allreduce_.SetValue(value, fixed);
}

int64_t ParameterManager::TensorFusionThresholdBytes() const {
  int64_t b = active_ ? joint_params_.Value()[0] : joint_params_.BestValue()[0];
  return b * 1024 * 1024;
};

void ParameterManager::SetTensorFusionThresholdBytes(int64_t threshold, bool fixed) {
  Eigen::VectorXd v = joint_params_.BestValue();
  v[0] = threshold / (1024 * 1024);
  joint_params_.SetValue(v, fixed);
}

double ParameterManager::CycleTimeMs() const {
  return active_ ? joint_params_.Value()[1] : joint_params_.BestValue()[1];
};

void ParameterManager::SetCycleTimeMs(double cycle_time_ms, bool fixed) {
  Eigen::VectorXd v = joint_params_.BestValue();
  v[1] = cycle_time_ms;
  joint_params_.SetValue(v, fixed);
}

void ParameterManager::Update(const std::vector<std::string>& tensor_names, int64_t bytes, double seconds) {
  if (!active_) {
    return;
  }

  for (const std::string& tensor_name : tensor_names) {
    int32_t cycle = tensor_counts_[tensor_name]++;
    if (cycle > cycle_) {
      scores_[cycle_] = total_bytes_ / total_seconds_;
      total_bytes_ = 0;
      total_seconds_ = 0;
      cycle_ = cycle;
      break;
    }
  }

  total_bytes_ += bytes;
  total_seconds_ += seconds;

  if (cycle_ >= CYCLES) {
    std::sort(scores_, scores_ + CYCLES);
    double med_score = scores_[CYCLES / 2];
    Tune(med_score);
  }
}

void ParameterManager::Tune(double score) {
  if (warmup_remaining_ > 0) {
    warmup_remaining_--;
    std::cerr << "WARMUP DONE | hierarchical tunable="
              << hierarchical_allreduce_.IsTunable() << " value=" << HierarchicalAllreduce() << std::endl;
  } else {
    std::cerr << "(" << rank_ << ") " << total_bytes_ << ", " << total_seconds_ << " "
              << "[" << joint_params_.Value()[1] << " ms , " << joint_params_.Value()[0] << " mb ] " << score << "  "
              << "[" << joint_params_.BestValue()[1] << " ms , " << joint_params_.BestValue()[0] << " mb] "
              << leaf_param_->BestScore()
              << std::endl;

    if (rank_ == root_rank_) {
      if (writing_ && file_.good()) {
        file_ << joint_params_.Value()[1] << "," << joint_params_.Value()[0] << "," << score << std::endl;
      }

      leaf_param_->Tune(score);
    }

    SyncParams();
  }
  ReadyTune();
}

void ParameterManager::ReadyTune() {
  total_bytes_ = 0;
  total_seconds_ = 0;
  tensor_counts_.clear();
  cycle_ = 0;
}

void ParameterManager::SyncParams() {
  Params params;
  if (rank_ == root_rank_) {
    params.hierarchical_allreduce = hierarchical_allreduce_.Value();
    params.tensor_fusion_threshold = joint_params_.Value()[0];
    params.cycle_time = joint_params_.Value()[1];
  }

  MPI_Bcast(&params, 1, mpi_params_type_, root_rank_, mpi_comm_);
  if (rank_ != root_rank_) {
    hierarchical_allreduce_.SetValue(params.hierarchical_allreduce, true);

    Eigen::VectorXd v(2);
    v(0) = params.tensor_fusion_threshold;
    v(1) = params.cycle_time;
    joint_params_.SetValue(v, true);
  }
}

// TunableParameter
template <class T>
ParameterManager::TunableParameter<T>::TunableParameter(
    T initial_value, ParameterManager &parent, ITunableParameter* const next_param) :
    initial_value_(initial_value),
    value_(initial_value),
    best_value_(initial_value),
    best_score_(0),
    tunable_(true),
    parent_(parent),
    next_param_(next_param) {}

template <class T>
void ParameterManager::TunableParameter<T>::Tune(double score) {
  if (!tunable_) {
    TuneNextParameter();
    return;
  }

  if (score > best_score_) {
    best_score_ = score;
    best_value_ = value_;
  }

  OnTune(score, value_);
  if (IsDoneTuning()) {
    CompleteTuning();
  }
}

template <class T>
void ParameterManager::TunableParameter<T>::SetValue(T value, bool fixed) {
  best_value_ = value;
  best_score_ = 0;

  if (fixed) {
    // TODO(tgaddair): this breaks Bayesian optimization as this makes all parameters constant
    value_ = value;
    tunable_ = false;
  }
}

template <class T>
void ParameterManager::TunableParameter<T>::SetCurrentValue(T value) {
  value_ = value;
}

template <class T>
void ParameterManager::TunableParameter<T>::TuneNextParameter() {
  if (next_param_ != nullptr) {
    next_param_->Tune(best_score_);
  } else {
    parent_.SetAutoTuning(false);
  }
}

template <class T>
void ParameterManager::TunableParameter<T>::CompleteTuning() {
  TuneNextParameter();
  value_ = initial_value_;
  ResetState();
}

// NumericParameter
template <class T>
ParameterManager::NumericParameter<T>::NumericParameter(
    T low, T high,
    ParameterManager& parent,
    ParameterManager::ITunableParameter* const next_param) :
    TunableParameter<T>(low, parent, next_param),
    low_init_(low),
    high_init_(high),
    low_(low),
    high_(high) {
  ResetState();
}

template <class T>
void ParameterManager::NumericParameter<T>::OnTune(double score, T& value) {
  if (std::isnan(left_.score)) {
    left_.score = score;
    value = right_.value;
  } else if (std::isnan(right_.score)) {
    right_.score = score;
  }

  if (!std::isnan(left_.score) && !std::isnan(right_.score)) {
    if (left_.score > right_.score) {
      high_ = right_.value;
      right_.value = left_.value;
      right_.score = left_.score;
      h_ = INVPHI * h_;
      value = low_ + INVPHI2 * h_;
      left_.value = value;
      left_.score = std::numeric_limits<double>::quiet_NaN();
      std::cerr << std::endl << "LEFT: " << value << " " << low_ << " " << left_.value << " " << right_.value << " " << high_ << std::endl << std::endl;
    } else {
      low_ = left_.value;
      left_.value = right_.value;
      left_.score = right_.score;
      h_ = INVPHI * h_;
      value = low_ + INVPHI * h_;
      right_.value = value;
      right_.score = std::numeric_limits<double>::quiet_NaN();
      std::cerr << std::endl << "LEFT: " << value << " " << low_ << " " << left_.value << " " << right_.value << " " << high_ << std::endl << std::endl;
    }

    k_++;
  }
}

template <class T>
bool ParameterManager::NumericParameter<T>::IsDoneTuning() const {
  return k_ >= n_ - 1;
}

template <class T>
void ParameterManager::NumericParameter<T>::ResetState() {
  low_ = low_init_;
  high_ = high_init_;
  h_ = high_ - low_;
  n_ = int32_t(ceil(log(TOL / h_) / log(INVPHI)));
  left_ = {low_ + INVPHI2 * h_, std::numeric_limits<double>::quiet_NaN()};
  right_ = {low_ + INVPHI * h_, std::numeric_limits<double>::quiet_NaN()};
  k_ = 0;
  this->SetCurrentValue(left_.value);
}

// CategoricalParameter
template <class T>
ParameterManager::CategoricalParameter<T>::CategoricalParameter(
    std::vector<T> values,
    ParameterManager& parent,
    ParameterManager::ITunableParameter* const next_param) :
    TunableParameter<T>(values[0], parent, next_param),
    values_(values) {
  ResetState();
}

template <class T>
void ParameterManager::CategoricalParameter<T>::OnTune(double score, T& value) {
  index_++;
  if (index_ < values_.size()) {
    value = values_[index_];
  }
}

template <class T>
bool ParameterManager::CategoricalParameter<T>::IsDoneTuning() const {
  return index_ >= values_.size();
}

template <class T>
void ParameterManager::CategoricalParameter<T>::ResetState() {
  index_ = 0;
}

// BayesianParameter
Eigen::VectorXd GetTestPoint(std::vector<std::pair<double, double>> bounds, double s) {
  Eigen::VectorXd v = Eigen::VectorXd::Zero(bounds.size());
  for (int i = 0; i < v.size(); i++) {
    double min = bounds[i].first;
    double max = bounds[i].second;
    v[i] = min + (max - min) * s;
  }
  return v;
}

ParameterManager::BayesianParameter::BayesianParameter(
    std::vector<std::pair<double, double>> bounds,
    ParameterManager& parent,
    ParameterManager::ITunableParameter* const next_param) :
    TunableParameter<Eigen::VectorXd>(GetTestPoint(bounds, 1.0 / 3.0), parent, next_param),
    bayes_(new BayesianOptimization(bounds.size(), bounds, 0.2)),
    bounds_(bounds),
    iteration_(0) {
  ResetState();
}

void ParameterManager::BayesianParameter::OnTune(double score, Eigen::VectorXd& value) {
  bayes_->AddSample(value, score);

  iteration_++;
  if (iteration_ > 1) {
    value = bayes_->NextSample();
  } else {
    value = GetTestPoint(bounds_, 2.0 / 3.0);
  }
}

bool ParameterManager::BayesianParameter::IsDoneTuning() const {
  return iteration_ > 50;
}

void ParameterManager::BayesianParameter::ResetState() {
  iteration_ = 0;
  bayes_->Clear();
}

} // namespace common
} // namespace horovod