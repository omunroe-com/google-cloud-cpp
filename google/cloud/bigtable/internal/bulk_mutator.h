// Copyright 2017 Google Inc.
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

#ifndef GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_BIGTABLE_INTERNAL_BULK_MUTATOR_H_
#define GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_BIGTABLE_INTERNAL_BULK_MUTATOR_H_

#include "google/cloud/bigtable/async_operation.h"
#include "google/cloud/bigtable/bigtable_strong_types.h"
#include "google/cloud/bigtable/completion_queue.h"
#include "google/cloud/bigtable/data_client.h"
#include "google/cloud/bigtable/idempotent_mutation_policy.h"
#include "google/cloud/bigtable/table_strong_types.h"
#include "google/cloud/internal/invoke_result.h"
#include "google/cloud/internal/make_unique.h"

namespace google {
namespace cloud {
namespace bigtable {
inline namespace BIGTABLE_CLIENT_NS {
namespace internal {
/// Keep the state in the Table::BulkApply() member function.
class BulkMutator {
 public:
  BulkMutator(bigtable::AppProfileId const& app_profile_id,
              bigtable::TableId const& table_name,
              IdempotentMutationPolicy& idempotent_policy, BulkMutation&& mut);

  /// Return true if there are pending mutations in the mutator
  bool HasPendingMutations() const {
    return pending_mutations_.entries_size() != 0;
  }

  /// Synchronously send one batch request to the given stub.
  grpc::Status MakeOneRequest(bigtable::DataClient& client,
                              grpc::ClientContext& client_context);

  /// Give up on any pending mutations, move them to the failures array.
  std::vector<FailedMutation> ExtractFinalFailures();

 protected:
  /// Get ready for a new request.
  void PrepareForRequest();

  /// Process a single response.
  void ProcessResponse(google::bigtable::v2::MutateRowsResponse& response);

  /// A request has finished and we have processed all the responses.
  void FinishRequest();

  /// Accumulate any permanent failures and the list of mutations we gave up on.
  std::vector<FailedMutation> failures_;

  /// The current request proto.
  google::bigtable::v2::MutateRowsRequest mutations_;

  /**
   * A small type to keep the annotations about pending mutations.
   *
   * As we process a MutateRows RPC we need to track the partial results for
   * each mutation in the request.  This object groups them in a small POD-type.
   */
  struct Annotations {
    /**
     * The index of this mutation in the original request.
     *
     * Each time the request is retried the operations might be reordered, but
     * we want to report any permanent failures using the index in the original
     * request provided by the application.
     */
    int original_index;
    bool is_idempotent;
    /// Set to false if the result is unknown.
    bool has_mutation_result;
  };

  /// The annotations about the current bulk request.
  std::vector<Annotations> annotations_;

  /// Accumulate mutations for the next request.
  google::bigtable::v2::MutateRowsRequest pending_mutations_;

  /// Accumulate annotations for the next request.
  std::vector<Annotations> pending_annotations_;
};

/**
 * Async-friendly version BulkMutator.
 *
 * It extends the normal BulkMutator with logic to do its job asynchronously.
 * Conceptually it reimplements MakeOneRequest in an async way.
 */
class AsyncBulkMutator : private BulkMutator {
 public:
  AsyncBulkMutator(std::shared_ptr<bigtable::DataClient> client,
                   bigtable::AppProfileId const& app_profile_id,
                   bigtable::TableId const& table_name,
                   IdempotentMutationPolicy& idempotent_policy,
                   BulkMutation&& mut)
      : BulkMutator(app_profile_id, table_name, idempotent_policy,
                    std::move(mut)),
        client_(std::move(client)) {}

  using Request = google::bigtable::v2::MutateRowsRequest;
  using Response = std::vector<FailedMutation>;

  template <typename Functor,
            typename std::enable_if<
                google::cloud::internal::is_invocable<Functor, CompletionQueue&,
                                                      grpc::Status&>::value,
                int>::type valid_callback_type = 0>
  void Start(CompletionQueue& cq,
             std::unique_ptr<grpc::ClientContext>&& context,
             Functor&& callback) {
    PrepareForRequest();
    cq.MakeUnaryStreamRpc(
        *client_, &DataClient::AsyncMutateRows, mutations_, std::move(context),
        [this](CompletionQueue&, const grpc::ClientContext&,
               google::bigtable::v2::MutateRowsResponse& response) {
          ProcessResponse(response);
        },
        FinishedCallback<Functor>(*this, std::forward<Functor>(callback)));
  }

  using BulkMutator::ExtractFinalFailures;
  using BulkMutator::HasPendingMutations;

 private:
  template <typename Functor,
            typename std::enable_if<
                google::cloud::internal::is_invocable<Functor, CompletionQueue&,
                                                      grpc::Status&>::value,
                int>::type valid_callback_type = 0>
  struct FinishedCallback {
    FinishedCallback(AsyncBulkMutator& parent, Functor&& callback)
        : parent_(parent), callback_(callback) {}

    void operator()(CompletionQueue& cq, grpc::ClientContext& context,
                    grpc::Status& status) {
      parent_.FinishRequest();
      callback_(cq, status);
    }

    // The user of AsyncBulkMutator has to make sure that it is not destructed
    // before all callbacks return, so we have a guarantee that this reference
    // is valid for as long as we don't call callback_.
    AsyncBulkMutator& parent_;
    Functor callback_;
  };

 private:
  std::shared_ptr<bigtable::DataClient> client_;
};

}  // namespace internal
}  // namespace BIGTABLE_CLIENT_NS
}  // namespace bigtable
}  // namespace cloud
}  // namespace google

#endif  // GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_BIGTABLE_INTERNAL_BULK_MUTATOR_H_
