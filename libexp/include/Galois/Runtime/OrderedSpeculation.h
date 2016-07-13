/** ?? -*- C++ -*-
 * @file
 * @section License
 *
 * This file is part of Galois.  Galoisis a framework to exploit
 * amorphous data-parallelism in irregular programs.
 *
 * Galois is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, version 2.1 of the
 * License.
 *
 * Galois is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Galois.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * @section Copyright
 *
 * Copyright (C) 2015, The University of Texas at Austin. All rights
 * reserved.
 *
 */

#ifndef GALOIS_RUNTIME_ORDERED_SPECULATION_H
#define GALOIS_RUNTIME_ORDERED_SPECULATION_H

#include "Galois/PerThreadContainer.h"
#include "Galois/PriorityQueue.h"
#include "Galois/DoAllWrap.h"
#include "Galois/Atomic.h"
#include "Galois/Accumulator.h"
#include "Galois/GaloisForwardDecl.h"
#include "Galois/optional.h"

#include "Galois/Runtime/Context.h"
#include "Galois/Runtime/OrderedLockable.h"
#include "Galois/Runtime/IKDGbase.h"
#include "Galois/Runtime/WindowWorkList.h"
#include "Galois/Runtime/UserContextAccess.h"
#include "Galois/Runtime/Executor_ParaMeter.h"
#include "Galois/Runtime/Mem.h"

#include "Galois/Substrate/gio.h"
#include "Galois/Substrate/PerThreadStorage.h"
#include "Galois/Substrate/CompilerSpecific.h"

namespace Galois {

namespace Runtime {

enum class SpecMode {
  OPTIM, PESSIM
};

namespace cll = llvm::cl;


cll::opt<SpecMode> specMode (
    cll::desc ("Speculation mode"),
    cll::values (
      clEnumVal (SpecMode::OPTIM, "SpecMode::OPTIM"),
      clEnumVal (SpecMode::PESSIM, "SpecMode::PESSIM"),
      clEnumValEnd),
    cll::init (SpecMode::OPTIM));

enum class ContextState: int {
    UNSCHEDULED = 0,
    SCHEDULED,
    READY_TO_COMMIT,
    ABORT_SELF,
    ABORT_HELP,
    COMMITTING, 
    COMMIT_DONE,
    READY_TO_ABORT,
    ABORTING,
    ABORT_DONE,
    ABORTED_CHILD,
    RECLAIM,
};

const char* ContextStateNames[] = {
    "UNSCHEDULED",
    "SCHEDULED",
    "READY_TO_COMMIT",
    "ABORT_SELF",
    "ABORT_HELP",
    "COMMITTING", 
    "COMMIT_DONE",
    "READY_TO_ABORT",
    "ABORTING",
    "ABORT_DONE",
    "ABORTED_CHILD",
    "RECLAIM",
};


template <typename Ctxt, typename CtxtCmp>
struct OptimNhoodItem: public OrdLocBase<OptimNhoodItem<Ctxt, CtxtCmp>, Ctxt, CtxtCmp> {

  using Base = OrdLocBase<OptimNhoodItem, Ctxt, CtxtCmp>;
  using Factory = OrdLocFactoryBase<OptimNhoodItem, Ctxt, CtxtCmp>;

  using Sharers = Galois::gstl::List<Ctxt*>;
  using Lock_ty = Galois::Substrate::SimpleLock;


  const CtxtCmp& ctxtCmp;
  GAtomic<Ctxt*> minCtxt;
  Sharers sharers;


  OptimNhoodItem (Lockable* l, const CtxtCmp& ctxtCmp): 
    Base (l), 
    ctxtCmp (ctxtCmp),
    minCtxt (nullptr)
  {}


  bool markMin (Ctxt* ctxt) {
    assert (ctxt);

    Ctxt* other = nullptr;

    do {

      other = minCtxt;

      if (other == ctxt) {
        return true;
      }

      if (other) {

        if (ctxtCmp (other, ctxt)) {

          ctxt->disableSrc ();
          return false;
        }
      }
      
    } while (!minCtxt.cas (other, ctxt));

    if (other) {
      other->disableSrc ();
    }

    return true;
  }

  Ctxt* getMin (void) const {
    return minCtxt;
  }

  void resetMin (Ctxt* c) {

    assert (getMin () == c);
    minCtxt = nullptr;
  }

  void addToHistory (Ctxt* ctxt) {

    assert (ctxt && ctxt->isSrc () && ctxt->hasState (ContextState::READY_TO_COMMIT));
    assert (std::find (sharers.begin (), sharers.end (), ctxt) == sharers.end ());

    if (!sharers.empty ()) {
      assert (sharers.back ()->hasState (ContextState::READY_TO_COMMIT));
    }
    sharers.push_back (ctxt);
  }

  Ctxt* getHistHead (void) const {

    if (sharers.empty ()) {
      return nullptr;

    } else {
      return sharers.front ();
    }
  }

  Ctxt* getHistTail (void) const {
    
    if (sharers.empty ()) {
      return nullptr;

    } else { 
      return sharers.back ();

    }
  }

  template <typename WL>
  bool findAborts (Ctxt* ctxt, WL& abortWL) {

    assert (getMin () == ctxt);

    bool ret = false;

    for (auto i = sharers.end (), beg_i = sharers.begin (); beg_i != i; ) {
      --i;
      if (ctxtCmp (ctxt, *i)) {
        dbg::print (ctxt, " causing sharer to abort ", *i);
        ret = true;
        (*i)->markForAbortRecursive (abortWL);

      } else {
        break;
      }
    }

    return ret;
  }

  //! mark all sharers later than ctxt for abort
  template <typename WL>
  void markForAbort (Ctxt* ctxt, WL& abortWL) {

    assert (std::find (sharers.begin (), sharers.end (), ctxt) != sharers.end ());

    bool succ = false;

    for (auto i = sharers.end (), beg_i = sharers.begin (); beg_i != i; ) {
      --i;
      if (ctxt == *i) {
        succ = true;
        break;

      } else {
        dbg::print (ctxt, " causing sharer to abort ", *i);
        (*i)->markForAbortRecursive (abortWL);
      }
    }

    assert (succ);

  }

  // TODO: re-implement
  void removeAbort (Ctxt* ctxt) {

    assert (!sharers.empty ());
    assert (std::find (sharers.begin (), sharers.end (), ctxt) != sharers.end ());

    assert (ctxt->hasState (ContextState::ABORTING));

    if (sharers.back () != ctxt) { 
      GALOIS_DIE ("invalid state");
    }

    assert (sharers.back () == ctxt);
    sharers.pop_back ();

    assert (std::find (sharers.begin (), sharers.end (), ctxt) == sharers.end ());

  }

  void removeCommit (Ctxt* ctxt) {


    assert (!sharers.empty ());
    assert (std::find (sharers.begin (), sharers.end (), ctxt) != sharers.end ());
    assert (sharers.front () == ctxt);

    sharers.pop_front ();

    assert (std::find (sharers.begin (), sharers.end (), ctxt) == sharers.end ());

  }

};


template <typename T, typename Cmp, typename Exec>
struct SpecContextBase: public OrderedContextBase<T> {

  using Base = OrderedContextBase<T>;
  using CtxtCmp = ContextComparator<SpecContextBase, Cmp>;
  using Executor = Exec;


  bool source;
  std::atomic<ContextState> state;
  Exec& exec;
  unsigned execRound;
  UserContextAccess<T> userHandle;

  explicit SpecContextBase (const T& x, const ContextState& s, Exec& exec)
  :
    Base (x), 
    source (true), 
    state (s), 
    exec (exec),
    execRound (0)
  {}

  bool hasState (const ContextState& s) const { return state == s; } 

  void setState (const ContextState& s) { state = s; } 

  bool casState (ContextState s_old, const ContextState& s_new) { 
    // return state.cas (s_old, s_new);
    return state.compare_exchange_strong (s_old, s_new);
  }

  void markExecRound (unsigned r) {
    assert (r >= execRound);
    execRound = r;
  }

  unsigned getExecRound (void) const {
    return execRound;
  }

  ContextState getState (void) const { return state; }

  void disableSrc (void) {
    source = false;
  }

  bool isSrc (void) const {
    return source; 
  }

  void schedule (void) {
    source = true;

    assert (hasState (ContextState::UNSCHEDULED) || hasState (ContextState::ABORT_DONE));
    setState (ContextState::SCHEDULED); 

    userHandle.reset ();
  }
};


template <typename T, typename Cmp, typename Exec>
struct OptimContext: public SpecContextBase<T, Cmp, Exec> {

  using Base = SpecContextBase<T, Cmp, Exec>;
  using CtxtCmp = typename Base::CtxtCmp;

  using NItem = OptimNhoodItem<OptimContext, CtxtCmp>;
  using NhoodMgr = PtrBasedNhoodMgr<NItem>;
  using NhoodList = typename gstl::Vector<NItem*>;
  using ChildList = typename gstl::Vector<OptimContext*>;

  Galois::GAtomic<bool> onWL;
  bool addBack; // set to false by parent when parent is marked for abort, see markAbortRecursive
  NhoodList nhood;

  // TODO: avoid using UserContextAccess and per-iteration allocator
  // use Pow of 2 block allocator instead. 
  ChildList children;


  explicit OptimContext (const T& x, const ContextState& s, Exec& exec)
  :
    Base (x, s, exec), 
    onWL (false),
    addBack (true)
  {}


  GALOIS_ATTRIBUTE_PROF_NOINLINE
  virtual void subAcquire (Lockable* l, Galois::MethodFlag m) {

    NItem& nitem = Base::exec.nhmgr.getNhoodItem (l);
    assert (NItem::getOwner (l) == &nitem);

    if (std::find (nhood.begin (), nhood.end (), &nitem) == nhood.end ()) {
      nhood.push_back (&nitem);
      nitem.markMin (this);
    }
  }

  void schedule (void) {
    Base::schedule ();
    onWL = false;
    addBack = true;
    nhood.clear ();
    children.clear ();
  }

  void publishChanges (void) {


    // for (auto i = userHandle.getPushBuffer ().begin (), 
        // end_i = userHandle.getPushBuffer ().end (); i != end_i; ++i) {
// 
      // OptimContext* child = exec.push (*i);
      // dbg::print (this, " creating child ", child);
// 
      // children.push_back (child);
    // }
  }

  void addChild (OptimContext* child) {

    assert (std::find (children.begin (), children.end (), child) == children.end ());

    dbg::print (this, " creating child ", child);

    children.push_back (child);

  }

  void doCommit () {

    assert (Base::hasState (ContextState::COMMITTING));

    dbg::print (this, " committing with item ", this->getActive ());

    Base::userHandle.commit (); // TODO: rename to retire

    for (NItem* n: nhood) {
      n->removeCommit (this);
    }

    Base::setState (ContextState::COMMIT_DONE);
  }

  void doAbort () {
    // this can be in states READY_TO_COMMIT, ABORT_SELF
    // children can be in UNSCHEDULED, READY_TO_COMMIT, ABORT_DONE

    // first abort all the children recursively
    // then abort self.
    //

    assert (Base::hasState (ContextState::ABORTING));

    dbg::print (this, " aborting with item ", this->getActive ());

    Base::userHandle.rollback ();

    for (NItem* ni: nhood) {
      ni->removeAbort (this);
    }

    if (this->addBack) {

      Base::setState (ContextState::ABORT_DONE);
      Base::exec.push_abort (this);

    } else {
      // is an aborted child whose parent also aborted
      Base::setState (ContextState::ABORTED_CHILD);
    }

  }

  bool isCommitSrc (void) const {

    for (const NItem* ni: nhood) {

      if (ni->getHistHead () != this) {
        return false;
      }
    }

    return true;
  }

  template <typename WL>
  void findCommitSrc (const OptimContext* gvt, WL& wl) const {

    for (const NItem* ni: nhood) {

      OptimContext* c = ni->getHistHead ();
      assert (c != this);

      if (c && (!gvt || Base::exec.ctxtCmp (c, gvt)) 
          && c->isCommitSrc () 
          && c->onWL.cas (false, true)) {
        wl.push (c);
      }
    }
  }

  bool isAbortSrc (void) const {

    if (!Base::hasState (ContextState::READY_TO_ABORT)) {
      return false;
    }

    for (const NItem* ni: nhood) {

      if (ni->getHistTail () != this) {
        return false;
      }
    }

    return true;
  }

  template <typename WL>
  void findAbortSrc (WL& wl) const {

    // XXX: if a task has children that don't share neighborhood with
    // it, should it be an abort source? Yes, because the end goal in 
    // finding abort sources is that tasks may abort and restore state
    // in isolation. 
    
    for (const NItem* ni: nhood) {

      OptimContext* c = ni->getHistTail ();

      if (c && c->isAbortSrc () && c->onWL.cas (false, true)) {
        wl.push (c);
      }
    }
  }

  bool isSrcSlowCheck (void) const {
    
    for (const NItem* ni: nhood) {

      if (ni->getMin () != this) {
        return false;
      }
    }

    return true;
  }

  template <typename WL>
  bool findAborts (WL& abortWL) {

    assert (isSrcSlowCheck ());

    bool ret = false;

    for (NItem* ni: nhood) {
      ret = ni->findAborts (this, abortWL) || ret;
    }

    return ret;
  }


  template <typename WL>
  void markForAbortRecursive (WL& abortWL) {
    if (Base::casState (ContextState::READY_TO_COMMIT, ContextState::READY_TO_ABORT)) {

      for (NItem* ni: nhood) {
        ni->markForAbort (this, abortWL);
      }

      if (isAbortSrc () && onWL.cas (false, true)) {
        abortWL.push (this);
      }

      for (OptimContext* c: children) {
        dbg::print (this, " causing abort on child ", c);
        c->markForAbortRecursive (abortWL);
        c->addBack = false;
      }

    } else if (Base::casState (ContextState::SCHEDULED, ContextState::ABORTED_CHILD)) {
      // a SCHEDULED task can only be aborted recursively if it's a child

    } else if (Base::casState (ContextState::UNSCHEDULED, ContextState::ABORTED_CHILD)) {

    } else {
      assert (Base::hasState (ContextState::READY_TO_ABORT) || Base::hasState (ContextState::ABORTED_CHILD));
    }

    assert (Base::hasState (ContextState::READY_TO_ABORT) || Base::hasState (ContextState::ABORTED_CHILD));

  }

  void resetMarks (void) {

    for (NItem* ni: nhood) {
      if (ni->getMin () == this) {
        ni->resetMin (this);
      }
    }
  }

  void addToHistory (void) {

    for (NItem* ni: nhood) {
      ni->addToHistory (this);
    }
  }

};

template <typename T, typename Cmp, typename NhFunc, typename ExFunc, typename OpFunc, typename ArgsTuple, typename Ctxt>
class OrdSpecExecBase: public IKDGbase<T, Cmp, NhFunc, ExFunc, OpFunc, ArgsTuple, Ctxt> {

 protected:

  using Base = IKDGbase <T, Cmp, NhFunc, ExFunc, OpFunc, ArgsTuple, Ctxt>;
  using Derived = typename Ctxt::Executor;

  using CtxtCmp = typename Ctxt::CtxtCmp;
  using CtxtWL = typename Base::CtxtWL;

  using WindowWL = typename std::conditional<Base::NEEDS_PUSH, PQwindowWL<Ctxt*, CtxtCmp>, SortedRangeWindowWL<Ctxt*, CtxtCmp> >::type;

  using CommitQ = Galois::PerThreadVector<Ctxt*>;
  using ExecutionRecords = std::vector<ParaMeter::StepStats>;

  static const unsigned DEFAULT_CHUNK_SIZE = 4;

  struct CtxtMaker {
    OrdSpecExecBase& outer;

    Ctxt* operator () (const T& x) {

      Ctxt* ctxt = outer.ctxtAlloc.allocate (1);
      assert (ctxt);
      outer.ctxtAlloc.construct (ctxt, x, ContextState::UNSCHEDULED, static_cast<Derived&> (outer));

      return ctxt;
    }
  };


  WindowWL winWL;
  CtxtMaker ctxtMaker;
  Substrate::PerThreadStorage<Ctxt*> currMinPending; // reset at the beginning of each round


  GAccumulator<size_t> totalRetires;
  CommitQ commitQ;
  ExecutionRecords execRcrds;
  TimeAccumulator t_beginRound;
  TimeAccumulator t_expandNhood;


public:
  OrdSpecExecBase (const Cmp& cmp, const NhFunc& nhFunc, const ExFunc& exFunc, const OpFunc& opFunc, const ArgsTuple& argsTuple)
    : 
      Base (cmp, nhFunc, exFunc, opFunc, argsTuple),
      winWL (Base::ctxtCmp),
      ctxtMaker {*this}
  {
  }

  ~OrdSpecExecBase (void) {
    dumpStats ();
  }

  template <typename R>
  void push_initial (const R& range) {

    StatTimer t ("push_initial");

    t.start ();

    Galois::do_all_choice (range,
        [this] (const T& x) {

          Ctxt* c = ctxtMaker (x);
          Base::getNextWL ().push (c);

        }, 
        std::make_tuple (
          Galois::loopname ("init-fill"),
          chunk_size<DEFAULT_CHUNK_SIZE> ()));

    if (Base::targetCommitRatio != 0.0) {

      winWL.initfill (makeLocalRange (Base::getNextWL ()));
      Base::getNextWL ().clear_all_parallel ();
    }

    t.stop ();

  }

protected:

  void dumpParaMeterStats (void) {
    // remove last record if its 0
    if (!execRcrds.empty() && execRcrds.back().parallelism.reduceRO () == 0) {
      execRcrds.pop_back();
    }

    for (const ParaMeter::StepStats& s: execRcrds) {
      s.dump (ParaMeter::getStatsFile (), Base::loopname);
    }

    ParaMeter::closeStatsFile ();
  }

  void dumpStats (void) {

    reportStat (Base::loopname, "retired", totalRetires.reduce (),0);
    reportStat (Base::loopname, "efficiency%", double (100 * totalRetires.reduce ()) / Base::totalTasks,0);
    reportStat (Base::loopname, "avg. parallelism", double (totalRetires.reduce ()) / Base::rounds, 0);
    reportStat ("NULL", "t_expandNhood",    t_expandNhood.get (),0);
    reportStat ("NULL", "t_beginRound",     t_beginRound.get (),0);

    if (Base::ENABLE_PARAMETER) {
      dumpParaMeterStats ();
    }
  }

  void updateCurrMinPending (Ctxt* c) {
    Ctxt*& minPending = *currMinPending.getLocal ();

    if (!minPending || Base::ctxtCmp (c, minPending)) {
      minPending = c;
    }
  }

  Ctxt* getMinWinWL (void) {
    Ctxt* m = nullptr;

    if (Base::NEEDS_PUSH) {
      if (Base::targetCommitRatio != 0.0 && !winWL.empty ()) {
        m = *winWL.getMin();
      }
    }

    return m;
  }

  Ctxt* getMinPending (void) {
    Ctxt* m = getMinWinWL ();

    for (unsigned i = 0; i < Galois::getActiveThreads (); ++i) {
      Ctxt* c = *currMinPending.getRemote (i);

      if (!c) { continue; }

      if (!m || Base::ctxtCmp (c, m)) {
        m = c;
      }
    }

    return m;
  }

  Ctxt* push_commit (const T& x, Ctxt* minWinWL, unsigned owner=Substrate::ThreadPool::getTID ()) {

    Ctxt* c = ctxtMaker (x); 
    assert (c);

    updateCurrMinPending (c);

    if (!minWinWL || Base::ctxtCmp (c, minWinWL)) {
        Base::getNextWL ().push_back (c, owner);

        dbg::print ("Child going to nextWL, c: ", c, ", with active: ", c->getActive ());
    } else {
      assert (!Base::ctxtCmp (c, minWinWL));
      assert (Base::targetCommitRatio != 0.0);
      winWL.push (c, owner);

      dbg::print ("Child going to winWL, c: ", c, ", with active: ", c->getActive ());
    }

    return c;
  }


  void push_abort (Ctxt* ctxt) {
    assert (ctxt);
    assert (ctxt->hasState (ContextState::ABORT_DONE));

    ctxt->setState (ContextState::UNSCHEDULED);

    updateCurrMinPending (ctxt);
    Base::getNextWL ().push (ctxt);

    Ctxt* m = getMinWinWL ();
    if (m) {
      assert (Base::ctxtCmp (ctxt, m));
    }
  }


  GALOIS_ATTRIBUTE_PROF_NOINLINE void beginRound () {

    t_beginRound.start ();
    
    Base::beginRound (winWL);

    if (Base::ENABLE_PARAMETER) {
      execRcrds.emplace_back (Base::rounds, Base::getCurrWL ().size_all ());
    }

    // reset currMinPending
    on_each_impl (
        [this] (const unsigned tid, const unsigned numT) {
          *(currMinPending.getLocal ()) = nullptr;
        });


    // TODO: remove after debugging
#ifndef NDEBUG
    const Ctxt* minWinWL = getMinWinWL ();
    const Ctxt* minCurrWL = Base::getMinCurrWL();
    const Ctxt* maxCurrWL = Base::getMaxCurrWL();

    if (minCurrWL) {
      dbg::print ("===== min CurrWL: ", minCurrWL, " with item: ", minCurrWL->getActive ());
    }
    if (maxCurrWL) {
      dbg::print ("max CurrWL: ", maxCurrWL, " with item: ", maxCurrWL->getActive ());
    }
    if (minWinWL) {
      dbg::print ("min Win WL: ", minWinWL, " with item: ", minWinWL->getActive ());
      assert (Base::ctxtCmp (maxCurrWL, minWinWL));
    }

#endif

    t_beginRound.stop ();
  }

  GALOIS_ATTRIBUTE_PROF_NOINLINE void expandNhood (void) {

    t_expandNhood.start ();

    Galois::do_all_choice (makeLocalRange (Base::getCurrWL ()),
        [this] (Ctxt* c) {

          if (!c->hasState (ContextState::ABORTED_CHILD)) {

            assert (!c->hasState (ContextState::RECLAIM));
            c->schedule ();

            dbg::print ("scheduling: ", c, " with item: ", c->getActive ());

            typename Base::UserCtxt& uhand = c->userHandle;

            // nhFunc (c, uhand);
            runCatching (Base::nhFunc, c, uhand);

            Base::roundTasks += 1;
          }
        },
        std::make_tuple (
          Galois::loopname ("expandNhood"),
          chunk_size<NhFunc::CHUNK_SIZE> ()));

    t_expandNhood.stop ();
  }


  void freeCtxt (Ctxt* ctxt) {
    Base::ctxtAlloc.destroy (ctxt);
    Base::ctxtAlloc.deallocate (ctxt, 1);
  }

  // FOR DEBUGGING
  /*
  Ctxt* computeGVT (void) {

    // t_computeGVT.start ();

    Substrate::PerThreadStorage<Ctxt*> perThrdMin;

    on_each_impl ([this, &perThrdMin] (const unsigned tid, const unsigned numT) {
          
          for (auto i = Base::getNextWL ().local_begin ()
            , end_i = Base::getNextWL ().local_end (); i != end_i; ++i) {

            Ctxt*& lm = *(perThrdMin.getLocal ());

            if (!lm || Base::ctxtCmp (*i, lm)) {
              lm = *i;
            }
          }

          
        });

    Ctxt* ret = nullptr;

    for (unsigned i = 0; i < perThrdMin.size (); ++i) {

      Ctxt* lm = *(perThrdMin.getRemote (i));

      if (lm) {
        if (!ret || Base::ctxtCmp (lm, ret)) {
          ret = lm;
        }
      }
    }

    Ctxt* const* minWinWL = winWL.getMin ();

    if (minWinWL) {
      if (!ret || Base::ctxtCmp (*minWinWL, ret)) {
        ret = *minWinWL;
      }
    }

    // t_computeGVT.stop ();

    return ret;

  }
  */
};

template <typename T, typename Cmp, typename NhFunc, typename ExFunc, typename  OpFunc, typename ArgsTuple>
class OptimOrdExecutor: public OrdSpecExecBase<T, Cmp, NhFunc, ExFunc, OpFunc, ArgsTuple,
  OptimContext<T, Cmp, OptimOrdExecutor<T, Cmp, NhFunc, ExFunc, OpFunc, ArgsTuple> > > {

protected:

  friend struct OptimContext<T, Cmp, OptimOrdExecutor>;
  using Ctxt = OptimContext<T, Cmp, OptimOrdExecutor>;
  using Base = OrdSpecExecBase<T, Cmp, NhFunc, ExFunc, OpFunc, ArgsTuple, Ctxt>;

  using NhoodMgr = typename Ctxt::NhoodMgr;
  using CtxtCmp = typename Ctxt::CtxtCmp;
  using NItemFactory = typename Ctxt::NItem::Factory;
  using CtxtWL = typename Base::CtxtWL;


  NItemFactory nitemFactory;
  NhoodMgr nhmgr;

  TimeAccumulator t_executeSources;
  TimeAccumulator t_applyOperator;
  TimeAccumulator t_serviceAborts;
  TimeAccumulator t_performCommits;
  TimeAccumulator t_reclaimMemory;

public:
  OptimOrdExecutor (const Cmp& cmp, const NhFunc& nhFunc, const ExFunc& exFunc, const OpFunc& opFunc, const ArgsTuple& argsTuple)
    : 
      Base (cmp, nhFunc, exFunc, opFunc, argsTuple),
      nitemFactory (Base::ctxtCmp),
      nhmgr (nitemFactory)
  {
  }

  ~OptimOrdExecutor (void) {
    reportStat ("NULL", "t_executeSources", t_executeSources.get (),0);
    reportStat ("NULL", "t_applyOperator",  t_applyOperator.get (),0);
    reportStat ("NULL", "t_serviceAborts",  t_serviceAborts.get (),0);
    reportStat ("NULL", "t_performCommits", t_performCommits.get (),0);
    reportStat ("NULL", "t_reclaimMemory",  t_reclaimMemory.get (),0);
  }

  void operator () (void) {
    execute ();
  }
  
  void execute () {

    StatTimer t ("executorLoop");

    typename Base::CtxtWL sources;

    t.start ();

    while (true) {

      Base::beginRound ();

      if (Base::getCurrWL ().empty_all ()) {
        break;
      }

      Base::expandNhood ();

      t_serviceAborts.start ();
      serviceAborts (sources);
      t_serviceAborts.stop ();

      t_executeSources.start ();
      executeSources (sources);
      t_executeSources.stop ();

      t_applyOperator.start ();
      applyOperator (sources);
      t_applyOperator.stop ();

      t_performCommits.start ();
      performCommits ();
      t_performCommits.stop ();

      t_reclaimMemory.start ();
      reclaimMemory (sources);
      t_reclaimMemory.stop ();

      Base::endRound ();

    }

    t.stop ();
  }

private:


  GALOIS_ATTRIBUTE_PROF_NOINLINE void executeSources (CtxtWL& sources) {

    if (Base::HAS_EXEC_FUNC) {


      Galois::do_all_choice (makeLocalRange (sources),
        [this] (Ctxt* ctxt) {
          assert (ctxt->isSrc ());
          assert (!ctxt->hasState (ContextState::RECLAIM));
          assert (!ctxt->hasState (ContextState::ABORTED_CHILD));

          Base::exFunc (ctxt->getActive (), ctxt->userHandle);
        },
        std::make_tuple (
          Galois::loopname ("executeSources"),
          Galois::chunk_size<ExFunc::CHUNK_SIZE> ()));

    }
  }

  GALOIS_ATTRIBUTE_PROF_NOINLINE void applyOperator (CtxtWL& sources) {

    Ctxt* minWinWL = Base::getMinWinWL ();


    Galois::do_all_choice (makeLocalRange (sources),
        [this, minWinWL] (Ctxt* c) {

          typename Base::UserCtxt& uhand = c->userHandle;

          assert (c->isSrc ());
          assert (!c->hasState (ContextState::RECLAIM));
          assert (!c->hasState (ContextState::ABORTED_CHILD));

          bool commit = true;

          if (Base::OPERATOR_CAN_ABORT) {
            runCatching (Base::opFunc, c, uhand);
            commit = c->isSrc (); // in case opFunc signalled abort

          } else {
            Base::opFunc (c->getActive (), uhand);
            commit = true;
          }

          if (commit) {

            if (Base::NEEDS_PUSH) {

              for (auto i = uhand.getPushBuffer ().begin ()
                  , endi = uhand.getPushBuffer ().end (); i != endi; ++i) {

                Ctxt* child = Base::push_commit (*i, minWinWL);
                c->addChild (child);

              }
            } else {

              assert (uhand.getPushBuffer ().begin () == uhand.getPushBuffer ().end ());
            }

            bool b = c->casState (ContextState::SCHEDULED, ContextState::READY_TO_COMMIT);

            assert (b && "CAS shouldn't have failed");
            Base::roundCommits += 1;

            c->publishChanges ();
            c->addToHistory ();
            Base::commitQ.get ().push_back (c);

            if (Base::ENABLE_PARAMETER) {
              c->markExecRound (Base::rounds);
            }

          } else {

            if (c->casState (ContextState::SCHEDULED, ContextState::ABORTING)) {
              c->doAbort ();

            } else {
              assert (c->hasState (ContextState::ABORTING) || c->hasState (ContextState::ABORT_DONE));
            }
          }
        },
        std::make_tuple (
          Galois::loopname ("applyOperator"),
          Galois::chunk_size<OpFunc::CHUNK_SIZE> ()));

  }


  void quickAbort (Ctxt* c) {
    assert (c);
    bool b= c->hasState (ContextState::SCHEDULED) || c->hasState (ContextState::ABORTED_CHILD) || c->hasState (ContextState::ABORT_DONE);

    assert (b);

    if (c->casState (ContextState::SCHEDULED, ContextState::ABORT_DONE)) {
      Base::push_abort (c);
      dbg::print("Quick Abort c: ", c, ", with active: ", c->getActive ());

    } else {
      assert (c->hasState (ContextState::ABORTED_CHILD)); 
    }
  }

  GALOIS_ATTRIBUTE_PROF_NOINLINE void serviceAborts (CtxtWL& sources) {

    CtxtWL abortWL;

    Galois::do_all_choice (makeLocalRange (Base::getCurrWL ()),
        [this, &abortWL] (Ctxt* c) {

          if (c->isSrc ()) {

            assert (c->isSrcSlowCheck ());

            if (c->findAborts (abortWL)) {
              // XXX: c does not need to abort if it's neighborhood
              // isn't dependent on values computed by other tasks
              

              c->disableSrc ();
              dbg::print("Causing rollbacks:", c, " with active: ", c->getActive ());
            }

          } 
        },
        std::make_tuple (
          Galois::loopname ("mark-aborts"),
          Galois::chunk_size<Base::DEFAULT_CHUNK_SIZE> ()));


    Galois::Runtime::for_each_gen (
        makeLocalRange (abortWL),
        [this] (Ctxt* c, UserContext<Ctxt*>& wlHandle) {

          if (c->casState (ContextState::READY_TO_ABORT, ContextState::ABORTING)) {
            c->doAbort ();
            c->findAbortSrc (wlHandle);
          
          } else {
            assert (c->hasState (ContextState::ABORTING) || c->hasState (ContextState::ABORT_DONE));
          }

          dbg::print("aborted after execution:", c, " with active: ", c->getActive ());
        },
        std::make_tuple (
          Galois::loopname ("handle-aborts"),
          Galois::does_not_need_aborts_tag (),
          Galois::wl<Galois::WorkList::dChunkedFIFO<NhFunc::CHUNK_SIZE> > ()));
    


    Galois::do_all_choice (makeLocalRange (Base::getCurrWL ()),

        [this, &sources] (Ctxt* c) {
          if (c->isSrc () && !c->hasState (ContextState::ABORTED_CHILD)) {
            assert (c->hasState (ContextState::SCHEDULED));

            sources.push (c);

          } else if (c->hasState (ContextState::ABORTED_CHILD)) {
            Base::commitQ.get ().push_back (c); // for reclaiming memory 

          } else {
            assert (!c->hasState (ContextState::ABORTED_CHILD));
            quickAbort (c);
          }

          c->resetMarks ();
        },
        std::make_tuple ( 
          Galois::loopname ("collect-sources"),
          Galois::chunk_size<Base::DEFAULT_CHUNK_SIZE> ()));

  }

  GALOIS_ATTRIBUTE_PROF_NOINLINE void performCommits () {

    CtxtWL commitSources;

    Ctxt* gvt = Base::getMinPending ();

    // TODO: remove this after debugging
    // Ctxt* gvtAlt = Base::computeGVT ();
// 
    // assert (gvt == gvtAlt);

    if (gvt) {
      dbg::print ("GVT computed as: ", gvt, ", with elem: ", gvt->getActive ());
    } else {
      dbg::print ("GVT computed as NULL");
    }



    Galois::do_all_choice (makeLocalRange (Base::commitQ),
        [this, gvt, &commitSources] (Ctxt* c) {

          assert (c);

          if (c->hasState (ContextState::READY_TO_COMMIT) 
              && (!gvt || Base::ctxtCmp (c, gvt))
              && c->isCommitSrc ()) {

            commitSources.push (c);
          }
        },
        std::make_tuple (
          Galois::loopname ("find-commit-srcs"),
          Galois::chunk_size<Base::DEFAULT_CHUNK_SIZE> ()));
        

    Galois::Runtime::for_each_gen (
        makeLocalRange (commitSources),
        [this, gvt] (Ctxt* c, UserContext<Ctxt*>& wlHandle) {

          bool b = c->casState (ContextState::READY_TO_COMMIT, ContextState::COMMITTING);

          if (b) {

            assert (c->isCommitSrc ());
            if (gvt) {
              assert (Base::ctxtCmp (c, gvt));
            }

            c->doCommit ();
            c->findCommitSrc (gvt, wlHandle);
            Base::totalRetires += 1;

            if (Base::ENABLE_PARAMETER) {
              assert (c->getExecRound () < Base::execRcrds.size ());
              Base::execRcrds[c->getExecRound ()].parallelism += 1;
            }

          } else {
            assert (c->hasState (ContextState::COMMIT_DONE));
          }
        },
        std::make_tuple (
          Galois::loopname ("retire"),
          Galois::does_not_need_aborts_tag (),
          Galois::wl<Galois::WorkList::dChunkedFIFO<Base::DEFAULT_CHUNK_SIZE> > ()));

  }

  void freeCtxt (Ctxt* ctxt) {
    Base::ctxtAlloc.destroy (ctxt);
    Base::ctxtAlloc.deallocate (ctxt, 1);
  }

  void reclaimMemory (CtxtWL& sources) {

    sources.clear_all_parallel ();

    // XXX: the following memory free relies on the fact that 
    // per-thread fixed allocators are being used. Otherwise, mem-free
    // should be done in a separate loop, after enforcing set semantics
    // among all threads


    Galois::Runtime::on_each_impl (
        [this] (const unsigned tid, const unsigned numT) {
          
          auto& localQ = Base::commitQ.get ();
          auto new_end = std::partition (localQ.begin (), 
            localQ.end (), 
            [] (Ctxt* c) {
              assert (c);
              return c->hasState (ContextState::READY_TO_COMMIT);
            });


          for (auto i = new_end, end_i = localQ.end (); i != end_i; ++i) {

            if ((*i)->casState (ContextState::ABORTED_CHILD, ContextState::RECLAIM)
              || (*i)->casState (ContextState::COMMIT_DONE, ContextState::RECLAIM)) {
              dbg::print ("Ctxt destroyed from commitQ: ", *i);
              freeCtxt (*i);
            }
          }

          localQ.erase (new_end, localQ.end ());
        });

  }




};


template <typename T, typename Cmp, typename Exec>
class PessimOrdContext: public SpecContextBase<T, Cmp, Exec> {

public:

  using Base = SpecContextBase<T, Cmp, Exec>;
  using NhoodList =  Galois::gstl::Vector<Lockable*>;
  using CtxtCmp = typename Base::CtxtCmp;
  using Executor = Exec;

  
  unsigned owner;
  NhoodList nhood;

  explicit PessimOrdContext (const T& x, const ContextState& s, Exec& e)
    : 
      Base (x, s, e), 
      owner (Substrate::ThreadPool::getTID ())

  {}

  void schedule () {
    Base::schedule ();
    nhood.clear ();
    owner = Substrate::ThreadPool::getTID ();
  }


  bool priorityAcquire (Lockable* l) {
    PessimOrdContext* other = nullptr;

    do {
      other = static_cast<PessimOrdContext*> (Base::getOwner (l));

      if (other == this) {
        return true;
      }

      if (other) {
        bool conflict = Base::exec.getCtxtCmp () (other, this); // *other < *this
        if (conflict) {
          // A lock that I want but can't get
          this->disableSrc ();
          return false; 
        }
      }
    } while (!this->CASowner(l, other));

    // Disable loser
    if (other) {
      other->disableSrc ();

      if (other->casState (ContextState::READY_TO_COMMIT, ContextState::ABORT_HELP)) {

        Base::exec.markForAbort (other);
        this->disableSrc ();// abort self to recompute after other has abortedthis->disableSrc ();
      } else if (other->hasState (ContextState::ABORT_HELP)) {
        this->disableSrc (); // abort self to recompute after other has aborted
      }

    }

    return true;
  }

  // TODO: Refactor common code with TwoPhaseContext::subAcquire
  virtual void subAcquire (Lockable* l, Galois::MethodFlag) {

    dbg::print (this, " trying to acquire ", l);

    if (std::find (nhood.cbegin (), nhood.cend (), l) == nhood.cend ()) {

      nhood.push_back (l);

      bool succ = priorityAcquire (l);

      if (succ) {
        dbg::print (this, " acquired lock ", l);
      } else {
        assert (!this->isSrc ());
        dbg::print (this, " failed to acquire lock ", l);
      }


    } // end if find

    return;
  }

  GALOIS_ATTRIBUTE_PROF_NOINLINE void doCommit () {
    assert (Base::hasState (ContextState::COMMITTING));

    // executor must already have pushed new work from userHandle.getPushBuffer
    // release locks
    dbg::print (this, " committing with item ", this->getActive ());

    Base::userHandle.commit ();
    releaseLocks ();
    Base::setState (ContextState::COMMIT_DONE);
  }

  GALOIS_ATTRIBUTE_PROF_NOINLINE void doAbort () {
    assert (Base::hasState (ContextState::ABORTING));
    // perform undo actions in reverse order
    // release locks
    // add active element to worklist
    dbg::print (this, " aborting with item ", this->getActive ());

    Base::userHandle.rollback ();
    releaseLocks ();
    Base::setState (ContextState::ABORT_DONE);
    Base::exec.push_abort (this);

  }

private:

  void releaseLocks () {
    for (Lockable* l: nhood) {
      assert (l != nullptr);
      if (Base::getOwner (l) == this) {
        dbg::print (this, " releasing lock ", l);
        Base::tryLock (l); // release requires having had the lock
        Base::release (l);
      }
    }

  }


};

template <typename T, typename Cmp, typename NhFunc, typename ExFunc, typename  OpFunc, typename ArgsTuple>
class PessimOrdExecutor: public OrdSpecExecBase<T, Cmp, NhFunc, ExFunc, OpFunc, ArgsTuple,
  PessimOrdContext<T, Cmp, PessimOrdExecutor<T, Cmp, NhFunc, ExFunc, OpFunc, ArgsTuple> > > {

protected:

  friend class PessimOrdContext<T, Cmp, PessimOrdExecutor>;
  using Ctxt = PessimOrdContext<T, Cmp, PessimOrdExecutor>;
  using Base = OrdSpecExecBase<T, Cmp, NhFunc, ExFunc, OpFunc, ArgsTuple, Ctxt>;

  using CtxtCmp = typename Ctxt::CtxtCmp;
  using CtxtWL = typename Base::CtxtWL;

  CtxtWL abortWL;

  TimeAccumulator t_executeSources;
  TimeAccumulator t_applyOperator;
  TimeAccumulator t_serviceAborts;
  TimeAccumulator t_performCommits;

public:

  PessimOrdExecutor (const Cmp& cmp, const NhFunc& nhFunc, const ExFunc& exFunc, const OpFunc& opFunc, const ArgsTuple& argsTuple)
    : 
      Base (cmp, nhFunc, exFunc, opFunc, argsTuple)
  {
  }

  ~PessimOrdExecutor (void) {
    reportStat ("NULL", "t_executeSources", t_executeSources.get (),0);
    reportStat ("NULL", "t_applyOperator",  t_applyOperator.get (),0);
    reportStat ("NULL", "t_serviceAborts",  t_serviceAborts.get (),0);
    reportStat ("NULL", "t_performCommits", t_performCommits.get (),0);
  }

  void markForAbort (Ctxt* c) {
    assert (c);
    abortWL.push (c);
  }

  void execute (void) {
    StatTimer t ("executorLoop");

    t.start ();

    while (true) {

      Base::beginRound ();

      if (Base::getCurrWL ().empty_all ()) {
        break;
      }

      Base::expandNhood ();

      t_serviceAborts.start ();
      serviceAborts ();
      t_serviceAborts.stop ();

      t_executeSources.start ();
      executeSources ();
      t_executeSources.stop ();

      t_applyOperator.start ();
      applyOperator ();
      t_applyOperator.stop ();

      t_performCommits.start ();
      performCommits ();
      t_performCommits.stop ();

      Base::endRound ();

    }

    t.stop ();

  }

  void serviceAborts () {

    Galois::do_all_choice (makeLocalRange (abortWL),
        [this] (Ctxt* c) {

          bool b = c->hasState (ContextState::ABORT_HELP) 
            || c->hasState (ContextState::ABORTING)
            || c->hasState (ContextState::ABORT_DONE);
          assert (b);

          if (c->casState (ContextState::ABORT_HELP, ContextState::ABORTING)) {
            c->doAbort ();
          }

        },
        std::make_tuple (
          Galois::loopname ("abort-marked"),
          Galois::chunk_size<Base::DEFAULT_CHUNK_SIZE> ()));

    abortWL.clear_all_parallel ();

  }

  GALOIS_ATTRIBUTE_PROF_NOINLINE void executeSources (void) {

    if (Base::HAS_EXEC_FUNC) {

      Galois::do_all_choice (makeLocalRange (Base::getCurrWL ()),
        [this] (Ctxt* ctxt) {

          if (ctxt->isSrc ()) {
            assert (ctxt->hasState (ContextState::SCHEDULED));
            Base::exFunc (ctxt->getActive (), ctxt->userHandle);
          }

        },
        std::make_tuple (
          Galois::loopname ("executeSources"),
          Galois::chunk_size<ExFunc::CHUNK_SIZE> ()));

    }
  }

  GALOIS_ATTRIBUTE_PROF_NOINLINE void applyOperator (void) {

    Galois::do_all_choice (makeLocalRange (Base::getCurrWL ()),
        [this] (Ctxt* c) {

          if (c->isSrc ()) {
            typename Base::UserCtxt& uhand = c->userHandle;

            bool commit = true;

            if (Base::OPERATOR_CAN_ABORT) {
              runCatching (Base::opFunc, c, uhand);
              commit = c->isSrc (); // in case opFunc signalled abort

            } else {
              Base::opFunc (c->getActive (), uhand);
              commit = true;
            }

            if (!commit) {
              bool b = c->casState (ContextState::SCHEDULED, ContextState::ABORTING);
              assert (b);

              c->doAbort ();

            } else {
              bool b = c->casState (ContextState::SCHEDULED, ContextState::READY_TO_COMMIT);
              assert (b);

              Base::commitQ.get ().push_back (c);
              Base::roundCommits += 1;

              if (Base::ENABLE_PARAMETER) {
                c->markExecRound (Base::rounds);
              }
            }

          } else {

            if (c->casState (ContextState::SCHEDULED, ContextState::ABORTING)) {
              c->doAbort ();

            } else {
              assert (c->hasState (ContextState::ABORTING) || c->hasState (ContextState::ABORT_DONE));
            }
          }
        },
        std::make_tuple (
          Galois::loopname ("applyOperator"),
          Galois::chunk_size<OpFunc::CHUNK_SIZE> ()));

  }

  void performCommits (void) {

    auto revCtxtCmp = [this] (const Ctxt* a, const Ctxt* b) { return Base::ctxtCmp (b, a); };

    Galois::Runtime::on_each_impl (
        [this, &revCtxtCmp] (const unsigned tid, const unsigned numT) {
          auto& localQ = Base::commitQ.get ();
          auto new_end = std::partition (localQ.begin (), 
            localQ.end (), 
            [] (Ctxt* c) {
              assert (c);
              return c->hasState (ContextState::READY_TO_COMMIT);
            });

          localQ.erase (new_end, localQ.end ());

          std::sort (localQ.begin (), localQ.end (), revCtxtCmp);
        });

    using C = typename Base::CommitQ::container_type;

    // assumes that per thread commit queues are sorted in reverse order
    auto qcmp = [this] (const C* q1, const C* q2) -> bool {

      assert (q1 && !q1->empty ());
      assert (q2 && !q2->empty ());

      return Base::ctxtCmp (q1->back (), q2->back ());
    };
     

    using PQ = Galois::MinHeap<C*, typename std::remove_reference<decltype (qcmp)>::type>;
    PQ commitMetaPQ (qcmp);

    for (unsigned i = 0; i < Galois::getActiveThreads (); ++i) {

      if (!Base::commitQ.get (i).empty ()) {
        commitMetaPQ.push (&(Base::commitQ.get (i))); 
      }
    }

    Ctxt* minWinWL = Base::getMinWinWL ();
    Ctxt* minPending = Base::getMinPending ();


    CtxtWL freeWL;

    while (!commitMetaPQ.empty ()) { 

      C* q = commitMetaPQ.pop ();
      assert (!q->empty ());

      bool e = commitMetaPQ.empty ();

      bool exit = false;

      do {
        Ctxt* c = q->back ();

        if (!minPending || !Base::ctxtCmp (minPending, c)) { // minPending >= c
          //do commit
          q->pop_back ();

          assert (c->hasState (ContextState::READY_TO_COMMIT) || c->hasState (ContextState::COMMIT_DONE));

          if (c->casState (ContextState::READY_TO_COMMIT, ContextState::COMMITTING)) {

            if (Base::NEEDS_PUSH) {
              auto& uhand = c->userHandle;
              for (auto i = uhand.getPushBuffer ().begin ()
                  , end_i = uhand.getPushBuffer ().end (); i != end_i; ++i) {

                Ctxt* child = Base::push_commit (*i, minWinWL, c->owner);

                if (!minPending || Base::ctxtCmp (child, minPending)) { // update minPending
                  minPending = child;
                }
              }
            }

            c->doCommit ();
            Base::totalRetires += 1;

            if (Base::ENABLE_PARAMETER) {
              assert (c->getExecRound () < Base::execRcrds.size ());
              Base::execRcrds[c->getExecRound ()].parallelism += 1;
            }


            freeWL.push (c, c->owner);
          }
            

        } else {
          exit = true;
          break; // exit
        }
      } while (!q->empty () && !e && qcmp (q, commitMetaPQ.top ()));

      if (exit) {
        break;
      }

      if (!q->empty ()) {
        commitMetaPQ.push (q);
      }
    } // end outer while


    // memory is returned to owner thread, thus thread 0 doesn't accumulate all 
    // the feed blocks
    on_each_impl (
        [this, &freeWL] (const unsigned tid, const unsigned numT) {
          for (auto i = freeWL.get ().begin ()
              , end_i = freeWL.get ().end (); i != end_i; ++i) {

            Base::freeCtxt (*i);
          }
        });
          
  } // end performCommits

};


template <template <typename, typename, typename, typename, typename, typename> class Executor, typename R, typename Cmp, typename NhFunc, typename ExFunc, typename OpFunc, typename _ArgsTuple>
void for_each_ordered_spec_impl (const R& range, const Cmp& cmp, const NhFunc& nhFunc, const ExFunc& exFunc, const OpFunc& opFunc, const _ArgsTuple& argsTuple) {


  auto argsT = std::tuple_cat (argsTuple, 
      get_default_trait_values (argsTuple,
        std::make_tuple (loopname_tag {}, enable_parameter_tag {}),
        std::make_tuple (default_loopname {}, enable_parameter<false> {})));
  using ArgsT = decltype (argsT);

  using T = typename R::value_type;
  

  using Exec = Executor<T, Cmp, NhFunc, ExFunc, OpFunc, ArgsT>;
  
  Exec e (cmp, nhFunc, exFunc, opFunc, argsT);

  Substrate::ThreadPool::getThreadPool().burnPower (Galois::getActiveThreads ());

  e.push_initial (range);
  e.execute ();

  Substrate::ThreadPool::getThreadPool().beKind();
}

template <typename R, typename Cmp, typename NhFunc, typename ExFunc, typename OpFunc, typename _ArgsTuple>
void for_each_ordered_optim (const R& range, const Cmp& cmp, const NhFunc& nhFunc, const ExFunc& exFunc, const OpFunc& opFunc, const _ArgsTuple& argsTuple) {

  for_each_ordered_spec_impl<OptimOrdExecutor> (range, cmp, nhFunc, exFunc, opFunc, argsTuple);
}

template <typename R, typename Cmp, typename NhFunc, typename OpFunc, typename _ArgsTuple>
void for_each_ordered_optim (const R& range, const Cmp& cmp, const NhFunc& nhFunc, const OpFunc& opFunc, const _ArgsTuple& argsTuple) {


  for_each_ordered_optim (range, cmp, nhFunc, HIDDEN::DummyExecFunc (), opFunc, argsTuple);
}

template <typename R, typename Cmp, typename NhFunc, typename ExFunc, typename OpFunc, typename _ArgsTuple>
void for_each_ordered_pessim (const R& range, const Cmp& cmp, const NhFunc& nhFunc, const ExFunc& exFunc, const OpFunc& opFunc, const _ArgsTuple& argsTuple) {

  for_each_ordered_spec_impl<PessimOrdExecutor> (range, cmp, nhFunc, exFunc, opFunc, argsTuple);
}

template <typename R, typename Cmp, typename NhFunc, typename OpFunc, typename _ArgsTuple>
void for_each_ordered_pessim (const R& range, const Cmp& cmp, const NhFunc& nhFunc, const OpFunc& opFunc, const _ArgsTuple& argsTuple) {

  for_each_ordered_pessim (range, cmp, nhFunc, HIDDEN::DummyExecFunc (), opFunc, argsTuple);
}


template <typename R, typename Cmp, typename NhFunc, typename ExFunc, typename OpFunc, typename _ArgsTuple>
void for_each_ordered_spec (const R& range, const Cmp& cmp, const NhFunc& nhFunc, const ExFunc& exFunc, const OpFunc& opFunc, const _ArgsTuple& argsTuple) {

  auto tplParam = std::tuple_cat (argsTuple, std::make_tuple (enable_parameter<true> ()));
  auto tplNoParam = std::tuple_cat (argsTuple, std::make_tuple (enable_parameter<false> ()));

  switch (specMode) {
    case SpecMode::OPTIM: {
      if (useParaMeterOpt) {
        for_each_ordered_spec_impl<OptimOrdExecutor> (range, cmp, nhFunc, exFunc, opFunc, tplParam);
      } else {
        for_each_ordered_spec_impl<OptimOrdExecutor> (range, cmp, nhFunc, exFunc, opFunc, tplNoParam);
      }
      break;
    }

    case SpecMode::PESSIM: {
      if (useParaMeterOpt) {
        for_each_ordered_spec_impl<PessimOrdExecutor> (range, cmp, nhFunc, exFunc, opFunc, tplParam);
      } else {
        for_each_ordered_spec_impl<PessimOrdExecutor> (range, cmp, nhFunc, exFunc, opFunc, tplNoParam);
      }
      break;
    }
    default:
      std::abort ();
  }
}

template <typename R, typename Cmp, typename NhFunc, typename OpFunc, typename _ArgsTuple>
void for_each_ordered_spec (const R& range, const Cmp& cmp, const NhFunc& nhFunc, const OpFunc& opFunc, const _ArgsTuple& argsTuple) {

  for_each_ordered_spec (range, cmp, nhFunc, HIDDEN::DummyExecFunc (), opFunc, argsTuple);
}



} // end namespace Runtime
} // end namespace Galois


#endif // GALOIS_RUNTIME_ORDERED_SPECULATION_H