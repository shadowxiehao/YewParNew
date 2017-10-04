#ifndef SKELETONS_BNB_INDEXED_HPP
#define SKELETONS_BNB_INDEXED_HPP

#include <vector>
#include <hpx/util/tuple.hpp>
#include <hpx/lcos/broadcast.hpp>
#include <hpx/runtime/components/new.hpp>
#include <hpx/include/serialization.hpp>

#include "registry.hpp"
#include "incumbent.hpp"

#include "util/func.hpp"
#include "util/positionIndex.hpp"

#include "workstealing/SearchManager.hpp"
#include "workstealing/SearchManagerScheduler.hpp"

namespace skeletons { namespace BnB { namespace Indexed {

template <typename Space,
          typename Sol,
          typename Bnd,
          typename Cand,
          typename Gen,
          typename Bound,
          bool PruneLevel = false>
struct BranchAndBoundOpt {
  using Response_t    = boost::optional<hpx::util::tuple<std::vector<unsigned>, int, hpx::naming::id_type> >;
  using SharedState_t = std::pair<std::atomic<bool>, hpx::lcos::local::one_element_channel<Response_t> >;

  static void expand(positionIndex & pos,
                     const hpx::util::tuple<Sol, Bnd, Cand> & n,
                     std::shared_ptr<SharedState_t> stealRequest) {
    constexpr bool const prunelevel = PruneLevel;

    // Handle Steal requests before processing a node
    if (std::get<0>(*stealRequest)) {
      std::get<1>(*stealRequest).set(pos.steal());
      std::get<0>(*stealRequest).store(false);
    }

    auto reg = skeletons::BnB::Components::Registry<Space, Sol, Bnd, Cand>::gReg;

    auto newCands = Gen::invoke(reg->space_, n);
    pos.setNumChildren(newCands.numChildren);

    auto i = 0;
    int nextPos;
    while ((nextPos = pos.getNextPosition()) >= 0) {
      auto c = newCands.next();

      if (nextPos != i) {
        for (auto j = 0; j < nextPos - i; ++j) {
          c = newCands.next();
        }
        i += nextPos - i;
      }

      auto lbnd = reg->localBound_.load();

      /* Prune if required */
      auto ubound = Bound::invoke(reg->space_, c);
      if (ubound <= lbnd) {
        if (prunelevel) {
          pos.pruneLevel();
          break; // numberChildren = 0?
        } else {
          ++i;
          continue;
        }
      }

      /* Update incumbent if required */
      if (hpx::util::get<1>(c) > lbnd) {
        skeletons::BnB::Components::updateRegistryBound<Space, Sol, Bnd, Cand>(hpx::util::get<1>(c));
        hpx::lcos::broadcast<updateRegistryBound_act>(hpx::find_all_localities(), hpx::util::get<1>(c));

        typedef typename bounds::Incumbent<Sol, Bnd, Cand>::updateIncumbent_action act;
        hpx::async<act>(reg->globalIncumbent_, c).get();
      }

      pos.preExpand(i);
      expand(pos, c, stealRequest);
      pos.postExpand();

      ++i;
    }
  }

  static hpx::util::tuple<Sol, Bnd, Cand> search(const Space & space,
                                                 const hpx::util::tuple<Sol, Bnd, Cand> & root) {

    // Initialise the registries on all localities
    auto bnd = hpx::util::get<1>(root);
    auto inc = hpx::new_<bounds::Incumbent<Sol, Bnd, Cand> >(hpx::find_here()).get();
    hpx::wait_all(hpx::lcos::broadcast<initRegistry_act>(hpx::find_all_localities(), space, bnd, inc, root));

    // Initialise the global incumbent
    typedef typename bounds::Incumbent<Sol, Bnd, Cand>::updateIncumbent_action updateInc;
    hpx::async<updateInc>(inc, root).get();

    // Initialise searchManagers on each locality to handle steals
    hpx::naming::id_type localSearchManager;
    std::vector<hpx::naming::id_type> searchManagers;
    for (auto const& loc : hpx::find_all_localities()) {
      auto mgr = hpx::new_<workstealing::SearchManager<std::vector<unsigned>, ChildTask> >(loc).get();
      searchManagers.push_back(mgr);
      if (loc == hpx::find_here()) {
        localSearchManager = mgr;
      }
    }

    // Start work stealing schedulers on all localities
    typedef typename workstealing::SearchManagerSched::startSchedulerAct<std::vector<unsigned>, ChildTask> startSchedulerAct;
    hpx::wait_all(hpx::lcos::broadcast<startSchedulerAct>(hpx::find_all_localities(), searchManagers));

    std::vector<unsigned> path;
    path.reserve(30);
    path.push_back(0);
    hpx::promise<void> prom;
    auto f = prom.get_future();
    auto pid = prom.get_id();

    typedef typename workstealing::SearchManager<std::vector<unsigned>, ChildTask>::addWork_action addWorkAct;
    hpx::async<addWorkAct>(localSearchManager, path, 1, pid);

    // Wait completion of the main task
    f.get();

    // Stop all work stealing schedulers
    hpx::wait_all(hpx::lcos::broadcast<stopScheduler_SearchManager_action>(hpx::find_all_localities()));

    // Read the result form the global incumbent
    typedef typename bounds::Incumbent<Sol, Bnd, Cand>::getIncumbent_action getInc;
    return hpx::async<getInc>(inc).get();
  }

  static hpx::util::tuple<Sol, Bnd, Cand> getStartingNode(std::vector<unsigned> path) {
    auto reg = skeletons::BnB::Components::Registry<Space, Sol, Bnd, Cand>::gReg;
    auto node =  reg->root_;


    // Paths have a leading 0 (representing root, we don't need this).
    path.erase(path.begin());

    if (path.empty()) {
      return node;
    }

    for (auto const & p : path) {
      auto newCands = Gen::invoke(reg->space_, node);
      node = newCands.nth(p);
    }

    return node;
  }

    static void searchChildTask(std::vector<unsigned> path,
                                const int depth,
                                std::shared_ptr<SharedState_t> stealRequest,
                                const hpx::naming::id_type doneProm,
                                const int idx,
                                const hpx::naming::id_type searchMgr) {
    auto posIdx = positionIndex(path);
    auto c = getStartingNode(path);

    expand(posIdx, c, stealRequest);

    typedef typename workstealing::SearchManager<std::vector<unsigned>, ChildTask>::done_action doneAct;
    hpx::async<doneAct>(searchMgr, idx).get();

    // Don't fully finish until we determine all children are also finished - Termination detection
    workstealing::SearchManagerSched::tasks_required_sem.signal();

    posIdx.waitFutures();
    hpx::async<hpx::lcos::base_lco_with_value<void>::set_value_action>(doneProm, true).get();
  }

  using ChildTask = func<
    decltype(&BranchAndBoundOpt<Space, Sol, Bnd, Cand, Gen, Bound, PruneLevel>::searchChildTask),
    &BranchAndBoundOpt<Space, Sol, Bnd, Cand, Gen, Bound, PruneLevel>::searchChildTask>;

};

}}}

#endif