/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/gpu/nccl_clique.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/node_hash_map.h"
#include "absl/functional/function_ref.h"
#include "absl/hash/hash.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/barrier.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "xla/debug_options_flags.h"
#include "xla/executable_run_options.h"
#include "xla/service/gpu/nccl_api.h"
#include "xla/service/gpu/nccl_clique_key.h"
#include "xla/service/lockable.h"
#include "xla/service/rendezvous.h"
#include "xla/status_macros.h"
#include "tsl/platform/env.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/logging.h"
#include "tsl/platform/statusor.h"

namespace xla::gpu {

//===----------------------------------------------------------------------===//
// NcclCliqueIdCallback
//===----------------------------------------------------------------------===//

bool IsGlobalNcclConfig() {
  static const char* const nccl_comm_id = std::getenv("NCCL_COMM_ID");
  return nccl_comm_id != nullptr;
}

absl::StatusOr<const NcclCliqueIdCallback*> GetNcclCliqueIdCallback(
    const NcclCliqueIdCallback* clique_id_callback, bool is_local) {
  if (clique_id_callback != nullptr) return clique_id_callback;

  TF_RET_CHECK(is_local || IsGlobalNcclConfig())
      << "If non-local devices are taking part of a collective API on "
         "GPU, the nccl_clique_id_callback must be provided by the client.";

  static auto* local_callback = new NcclCliqueIdCallback(
      [](const NcclCliqueKey&) { return NcclApi::Default()->GetUniqueId(); });
  return local_callback;
}

//===----------------------------------------------------------------------===//
// NcclClique Acquire and Initialization Timeouts
//===----------------------------------------------------------------------===//

// We rely on rendezvous (all participating threads arriving to a rendezvous
// barrier at the same time) to guarantee that NCCL communicators used in a way
// that does not lead to a deadlock. This itself can create new deadlocks if
// thread pools sized incorrectly. To prevent hard to debug deadlocks with WARN
// and terminate when detect that rendezvous runs for too long.

static absl::Duration WarnStuckTimeout() { return absl::Seconds(10); }

static absl::Duration TerminateTimeout() {
  static const int64_t terminate_timeout =
      xla::GetDebugOptionsFromFlags()
          .xla_gpu_nccl_termination_timeout_seconds();
  return (terminate_timeout >= 0) ? absl::Seconds(terminate_timeout)
                                  : absl::InfiniteDuration();
}

//===----------------------------------------------------------------------===//
// NcclClique
//===----------------------------------------------------------------------===//

NcclCliqueCommunicators::NcclCliqueCommunicators(
    NcclCliqueKey clique_key, NcclCliqueId clique_id,
    absl::node_hash_map<int32_t, NcclComm> communicators)
    : clique_key_(std::move(clique_key)),
      clique_id_(std::move(clique_id)),
      communicators_(std::move(communicators)) {}

std::optional<NcclComm*> NcclCliqueCommunicators::comm(int32_t rank) {
  if (auto it = communicators_.find(rank); it != communicators_.end()) {
    return &it->second;
  }
  return std::nullopt;
}

void NcclCliqueCommunicators::ForEachComm(
    absl::FunctionRef<void(int32_t, NcclComm&)> fn) {
  for (auto& [rank, comm] : communicators_) {
    fn(rank, comm);
  }
}

std::string NcclCliqueCommunicators::DebugString() const {
  std::string out = absl::StrFormat(
      "clique_key: %s; hash(id): %d; size: %d; communicators: ",
      clique_key_.ToString(), absl::HashOf(clique_id_), communicators_.size());
  int32_t cnt = 0;
  for (const auto& [rank, comm] : communicators_) {
    if (cnt++) absl::StrAppend(&out, ", ");
    absl::StrAppendFormat(&out, "[rank=%d, comm=%p]", rank, comm.value());
  }
  return out;
}

std::string NcclClique::DebugString() const {
  return absl::StrFormat("NcclClique: %s", value().DebugString());
}

namespace {
// Container for initialized and ready to use local (in-process) NCCL cliques.
struct NcclCliques {
  absl::Mutex mu;
  absl::node_hash_map<NcclCliqueKey, NcclClique> map ABSL_GUARDED_BY(mu);
};
}  // namespace

// Returns local (in-process) NcclCliques container.
static NcclCliques& GetNcclCliques() {
  static auto* cliques = new NcclCliques;
  return *cliques;
}

// Acquires a NCCL clique for a given key. Should be used with extra care if
// executed outside of a rendezvous callback as it's unsafe to launch unrelated
// collective operations using the same clique out of order.
//
// If NCCL clique for a given key is not initialized returns an empty lock.
// Caller must always check if lock is valid before trying to use it.
static absl::StatusOr<NcclClique::Lock> AcquireNcclClique(
    const NcclCliqueKey& clique_key, RunId run_id,
    int32_t num_local_participants) {
  NcclCliques& cliques = GetNcclCliques();

  absl::MutexLock lock(&cliques.mu);
  if (auto it = cliques.map.find(clique_key); it != cliques.map.end()) {
    return it->second.Acquire();
  }

  // Return empty lock if we do not have a clique for `clique_key`.
  return NcclClique::Lock();
}

//===----------------------------------------------------------------------===//
// NcclClique Heart Beat Monitor
//===----------------------------------------------------------------------===//

// Runs an async error check for a `comm` and aborts it if it is in the
// error state. It will free resources that are allocated to a communicator
// and abort any uncompleted operations before destroying the communicator.
static absl::Status CheckComm(NcclComm& lockable_comm) {
  if (NcclComm::Lock comm = lockable_comm.TryAcquire()) {
    absl::Status async_err = NcclApi::Default()->CommGetAsyncError(*comm);
    if (!async_err.ok()) {
      LOG(ERROR) << "Aborting communicator: " << comm
                 << " due to async NCCL error: " << async_err;
      TF_RETURN_IF_ERROR(NcclApi::Default()->CommAbort(*comm));
    }
    return async_err;
  }
  return absl::OkStatus();
}

// Runs async check on all communicators in a clique.
static void CheckClique(const NcclCliqueKey& clique_key,
                        NcclClique& lockable_clique) {
  if (NcclClique::Lock clique = lockable_clique.TryAcquire()) {
    VLOG(5) << "Checking NCCL clique " << clique_key.ToString()
            << " for async errors; num_communicators=" << clique->size();
    clique->ForEachComm([](int32_t rank, NcclComm& comm) {
      if (auto status = CheckComm(comm); !status.ok()) LOG(ERROR) << status;
    });
  } else {
    VLOG(5) << "Skip checking in-use NCCL clique " << clique_key.ToString();
  }
}

// TODO(ezhulenev): We need a mechanism to destroy whole clique when one of the
// communicators is aborted to be able to recover from errors.
static void NcclCliqueHeartBeatMonitorThread() {
  VLOG(5) << "Starting NCCL clique heart beat monitor";
  while (true) {
    absl::SleepFor(absl::Seconds(30));
    NcclCliques& cliques = GetNcclCliques();
    absl::MutexLock lock(&cliques.mu);
    VLOG(5) << "Checking NCCL communicators for async errors"
            << "; num_cliques=" << cliques.map.size();
    for (auto& [clique_key, lockable_clique] : cliques.map) {
      CheckClique(clique_key, lockable_clique);
    }
  }
}

static void StartNcclCliqueHeartBeatMonitor() {
  static auto* monitor_thread = tsl::Env::Default()->StartThread(
      tsl::ThreadOptions(), "nccl_clique_heart_beat_monitor",
      NcclCliqueHeartBeatMonitorThread);
  (void)monitor_thread;  // suppress unused variable warning
}

//===----------------------------------------------------------------------===//
// NcclClique Initialization
//===----------------------------------------------------------------------===//

// NcclClique initialization must be executed together by all participants, and
// we rely on rendezvous to guarantee that all ranks are ready to initialize
// NCCL communicators. We also have an additional level of synchronization to
// guarantee that we do at most one clique initialization at a time.

namespace {

struct InitializationGuardName {
  static std::string ToString(std::nullopt_t) {
    return absl::StrFormat("clique initilization guard");
  }
};

// An in-process singleton that guards collective cliques initialization as
// multiple concurrent initializations lead to deadlocks (in NCCL).
struct InitializationGuard
    : public Lockable<std::nullopt_t, InitializationGuardName> {
  InitializationGuard() : Lockable(std::nullopt) {}
};

// Local (in-process) NCCL clique initialization state. Once initialization is
// complete NCCL clique added to the NcclCliques container (see above).
struct InitializationState {
  using Ranks = absl::Span<const int32_t* const>;
  InitializationState(NcclCliqueId clique_id, Ranks ranks,
                      InitializationGuard::Lock lock);

  NcclCliqueId clique_id;
  absl::node_hash_map<int32_t, absl::StatusOr<NcclApi::OwnedNcclComm>> comms;

  // Guards initialization of NCCL cliques.
  InitializationGuard::Lock lock;

  // Signals when all participants updated entries in `comms`.
  std::unique_ptr<absl::Barrier> ready;
};

}  // namespace

static InitializationGuard::Lock AcquireInitializationGuard() {
  static auto* guard = new InitializationGuard();
  return guard->Acquire();
}

InitializationState::InitializationState(NcclCliqueId clique_id, Ranks ranks,
                                         InitializationGuard::Lock lock)
    : clique_id(clique_id),
      lock(std::move(lock)),
      ready(new absl::Barrier(ranks.size())) {
  // Initialize `comms` for all ranks so that each participating thread can
  // write into it without synchronization.
  for (const int32_t* rank : ranks) {
    comms[*rank] = absl::InternalError("uninitialized NCCL communicator");
  }
}

// Creates a new NCCL communicator for a given `rank` and joins a rendezvous to
// initialize a clique for a `clique_key`. Returns a lock that gives exclusive
// access to a NCCL clique.
static absl::StatusOr<std::shared_ptr<NcclClique::Lock>> InitializeNcclClique(
    RunId run_id, NcclCliqueKey clique_key,
    const NcclCliqueIdCallback& clique_id_callback,
    int32_t num_local_participants, int32_t rank) {
  int nranks = clique_key.devices().size();
  VLOG(3) << "Initialize NCCL clique " << clique_key.ToString() << " rank #"
          << rank << " of " << nranks
          << "; num_local_participants=" << num_local_participants;

  // Start NCCL clique heart beat monitor when create a first clique.
  StartNcclCliqueHeartBeatMonitor();

  // Creates initialization state for participating ranks.
  auto create_initialization_state = [&](absl::Span<const int32_t* const> ranks)
      -> absl::StatusOr<InitializationState> {
    VLOG(3) << "Acquire initialization guard for clique "
            << clique_key.ToString();
    auto lock = AcquireInitializationGuard();
    TF_ASSIGN_OR_RETURN(auto clique_id, clique_id_callback(clique_key));
    VLOG(3) << "Created unique clique id (hash): " << absl::HashOf(clique_id);
    return InitializationState(clique_id, ranks, std::move(lock));
  };

  // We include `run_id` to a rendezvous key to make sure that multiple
  // concurrent initializations will not join the same rendezvous. The winner
  // will update cliques state, and others will destroy unused communicators.
  auto rendezvous_key = std::make_tuple(run_id, clique_key);
  auto initialization_rendezvous_name = absl::StrFormat(
      "create clique initialization state for rank %d; clique=%s; run_id=%d",
      rank, clique_key.ToString(), run_id.ToInt());

  // Do a round of rendezvous to wait for all participants to join NCCL clique
  // initialization process.
  TF_ASSIGN_OR_RETURN(std::shared_ptr<InitializationState> state,
                      RendezvousSingle<absl::StatusOr<InitializationState>>(
                          initialization_rendezvous_name, rendezvous_key, rank,
                          num_local_participants, create_initialization_state,
                          WarnStuckTimeout(), TerminateTimeout()));

  VLOG(3) << "Create NCCL communicator for clique " << clique_key.ToString()
          << " rank #" << rank << " of " << nranks;

  absl::StatusOr<NcclApi::OwnedNcclComm> comm =
      NcclApi::Default()->CommInitRank(nranks, state->clique_id, rank);

  if (comm.ok()) {
    VLOG(3) << "Created NCCL communicator for clique " << clique_key.ToString()
            << " rank #" << rank << " of " << nranks;
    state->comms[rank] = std::move(*comm);
  } else {
    VLOG(3) << "Failed to create NCCL communicator for clique "
            << clique_key.ToString() << " rank #" << rank << " of " << nranks
            << "; error=" << comm.status();
    state->comms[rank] = comm.status();
  }

  // Wait for all participants to complete communicator initialization.
  bool completed_initialization = state->ready->Block();

  // Check that all ranks successfully initialize communicators.
  for (const auto& [rank, comm] : state->comms) {
    TF_RETURN_IF_ERROR(comm.status());
  }

  // If we are the leader who completed the clique initialization we should
  // update the local (in-process) cliques state.
  if (completed_initialization) {
    NcclCliques& cliques = GetNcclCliques();

    // Create NCCL communicators from handles.
    absl::node_hash_map<int32_t, NcclComm> communicators;
    for (auto& [rank, comm] : state->comms) {
      if (*comm == nullptr) {
        return absl::InternalError(absl::StrFormat(
            "uninitialized NCCL communicator for rank %d", rank));
      }
      communicators.try_emplace(rank, comm->release());
    }

    VLOG(3) << "Completed NCCL clique initialization for a clique "
            << clique_key.ToString();

    // Create a new clique with given clique id and communicators.
    absl::MutexLock lock(&cliques.mu);
    auto emplaced = cliques.map.try_emplace(
        clique_key, clique_key, state->clique_id, std::move(communicators));

    // We can have a race to create a clique for a given key, the winner inserts
    // it into a map and the looser destroys all communicators.
    if (!emplaced.second) {
      VLOG(3) << "Clique already exists: "
              << emplaced.first->second.DebugString();
    } else {
      VLOG(3) << "Created new clique: " << emplaced.first->second.DebugString();
    }
  }

  // Do one more round of rendezvous to guarantee that all ranks that
  // participated in clique initialization will share an exclusive access to all
  // communicators in a NCCL clique.
  auto initialized_rendezvous_name = absl::StrFormat(
      "acquire initialized clique for rank %d; clique=%s; run_id=%d", rank,
      clique_key.ToString(), run_id.ToInt());

  return RendezvousSingle<absl::StatusOr<NcclClique::Lock>>(
      initialized_rendezvous_name, rendezvous_key, num_local_participants,
      [&] {
        return AcquireNcclClique(clique_key, run_id, num_local_participants);
      },
      WarnStuckTimeout(), TerminateTimeout());
}

//===----------------------------------------------------------------------===//

absl::StatusOr<std::shared_ptr<NcclClique::Lock>> AcquireNcclClique(
    RunId run_id, NcclCliqueKey clique_key,
    const NcclCliqueIdCallback& clique_id_callback, int32_t rank,
    size_t num_local_participants, bool may_skip_rendezvous) {
  VLOG(2) << "Acquire NCCL clique " << clique_key.ToString() << "; run"
          << run_id.ToString() << "; rank " << rank
          << "; num_local_participants=" << num_local_participants
          << "; may_skip_rendezvous=" << may_skip_rendezvous;

  // If we prefer to skip rendezvous check if NcclClique is already available
  // for a given key.
  // TODO(ezhulenev): Remove this code path as it leads to deadlocks.
  if (may_skip_rendezvous) {
    TF_ASSIGN_OR_RETURN(
        NcclClique::Lock clique,
        AcquireNcclClique(clique_key, run_id, num_local_participants));

    // If lock is not null return it to the caller.
    if (clique) return std::make_shared<NcclClique::Lock>(std::move(clique));

  } else {
    // Get the clique lock via the rendezvous process.
    auto rendezvous_key = std::make_tuple(run_id, clique_key);
    auto rendezvous_name =
        absl::StrFormat("acquire clique for rank %d; clique=%s; run_id=%d",
                        rank, clique_key.ToString(), run_id.ToInt());

    TF_ASSIGN_OR_RETURN(
        std::shared_ptr<NcclClique::Lock> clique,
        RendezvousSingle<absl::StatusOr<NcclClique::Lock>>(
            rendezvous_name, rendezvous_key, num_local_participants,
            [&] {
              return AcquireNcclClique(clique_key, run_id,
                                       num_local_participants);
            },
            WarnStuckTimeout(), TerminateTimeout()));

    // If lock is not null return it to the caller.
    if (*clique) return clique;
  }

  // If NCCL clique is not found try to initialize a new one for a given key.
  return InitializeNcclClique(run_id, clique_key, clique_id_callback,
                              num_local_participants, rank);
}

}  // namespace xla::gpu
