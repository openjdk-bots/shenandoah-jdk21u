/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"

#include "gc/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahGenerationalControlThread.hpp"
#include "gc/shenandoah/shenandoahGenerationalEvacuationTask.hpp"
#include "gc/shenandoah/shenandoahGenerationalHeap.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.hpp"
#include "gc/shenandoah/shenandoahInitLogger.hpp"
#include "gc/shenandoah/shenandoahMemoryPool.hpp"
#include "gc/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahOopClosures.inline.hpp"
#include "gc/shenandoah/shenandoahPhaseTimings.hpp"
#include "gc/shenandoah/shenandoahRegulatorThread.hpp"
#include "gc/shenandoah/shenandoahScanRemembered.inline.hpp"
#include "gc/shenandoah/shenandoahWorkerPolicy.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "logging/log.hpp"
#include "utilities/events.hpp"


class ShenandoahGenerationalInitLogger : public ShenandoahInitLogger {
public:
  static void print() {
    ShenandoahGenerationalInitLogger logger;
    logger.print_all();
  }

  void print_heap() override {
    ShenandoahInitLogger::print_heap();

    ShenandoahGenerationalHeap* heap = ShenandoahGenerationalHeap::heap();

    ShenandoahYoungGeneration* young = heap->young_generation();
    log_info(gc, init)("Young Generation Soft Size: " EXACTFMT, EXACTFMTARGS(young->soft_max_capacity()));
    log_info(gc, init)("Young Generation Max: " EXACTFMT, EXACTFMTARGS(young->max_capacity()));

    ShenandoahOldGeneration* old = heap->old_generation();
    log_info(gc, init)("Old Generation Soft Size: " EXACTFMT, EXACTFMTARGS(old->soft_max_capacity()));
    log_info(gc, init)("Old Generation Max: " EXACTFMT, EXACTFMTARGS(old->max_capacity()));
  }

protected:
  void print_gc_specific() override {
    ShenandoahInitLogger::print_gc_specific();

    ShenandoahGenerationalHeap* heap = ShenandoahGenerationalHeap::heap();
    log_info(gc, init)("Young Heuristics: %s", heap->young_generation()->heuristics()->name());
    log_info(gc, init)("Old Heuristics: %s", heap->old_generation()->heuristics()->name());
  }
};

ShenandoahGenerationalHeap* ShenandoahGenerationalHeap::heap() {
  shenandoah_assert_generational();
  CollectedHeap* heap = Universe::heap();
  return checked_cast<ShenandoahGenerationalHeap*>(heap);
}

size_t ShenandoahGenerationalHeap::calculate_min_plab() {
  return align_up(PLAB::min_size(), CardTable::card_size_in_words());
}

size_t ShenandoahGenerationalHeap::calculate_max_plab() {
  size_t MaxTLABSizeWords = ShenandoahHeapRegion::max_tlab_size_words();
  return ((ShenandoahMaxEvacLABRatio > 0)?
          align_down(MIN2(MaxTLABSizeWords, PLAB::min_size() * ShenandoahMaxEvacLABRatio), CardTable::card_size_in_words()):
          align_down(MaxTLABSizeWords, CardTable::card_size_in_words()));
}

// Returns size in bytes
size_t ShenandoahGenerationalHeap::unsafe_max_tlab_alloc(Thread *thread) const {
  return MIN2(ShenandoahHeapRegion::max_tlab_size_bytes(), young_generation()->available());
}

ShenandoahGenerationalHeap::ShenandoahGenerationalHeap(ShenandoahCollectorPolicy* policy) :
  ShenandoahHeap(policy),
  _min_plab_size(calculate_min_plab()),
  _max_plab_size(calculate_max_plab()),
  _regulator_thread(nullptr),
  _young_gen_memory_pool(nullptr),
  _old_gen_memory_pool(nullptr) {
  assert(is_aligned(_min_plab_size, CardTable::card_size_in_words()), "min_plab_size must be aligned");
  assert(is_aligned(_max_plab_size, CardTable::card_size_in_words()), "max_plab_size must be aligned");
}

void ShenandoahGenerationalHeap::print_init_logger() const {
  ShenandoahGenerationalInitLogger logger;
  logger.print_all();
}

void ShenandoahGenerationalHeap::initialize_serviceability() {
  assert(mode()->is_generational(), "Only for the generational mode");
  _young_gen_memory_pool = new ShenandoahYoungGenMemoryPool(this);
  _old_gen_memory_pool = new ShenandoahOldGenMemoryPool(this);
  cycle_memory_manager()->add_pool(_young_gen_memory_pool);
  cycle_memory_manager()->add_pool(_old_gen_memory_pool);
  stw_memory_manager()->add_pool(_young_gen_memory_pool);
  stw_memory_manager()->add_pool(_old_gen_memory_pool);
}

GrowableArray<MemoryPool*> ShenandoahGenerationalHeap::memory_pools() {
  assert(mode()->is_generational(), "Only for the generational mode");
  GrowableArray<MemoryPool*> memory_pools(2);
  memory_pools.append(_young_gen_memory_pool);
  memory_pools.append(_old_gen_memory_pool);
  return memory_pools;
}

void ShenandoahGenerationalHeap::initialize_controller() {
  auto control_thread = new ShenandoahGenerationalControlThread();
  _control_thread = control_thread;
  _regulator_thread = new ShenandoahRegulatorThread(control_thread);
}

void ShenandoahGenerationalHeap::gc_threads_do(ThreadClosure* tcl) const {
  if (!shenandoah_policy()->is_at_shutdown()) {
    ShenandoahHeap::gc_threads_do(tcl);
    tcl->do_thread(regulator_thread());
  }
}

void ShenandoahGenerationalHeap::stop() {
  regulator_thread()->stop();
  ShenandoahHeap::stop();
}

oop ShenandoahGenerationalHeap::evacuate_object(oop p, Thread* thread) {
  assert(thread == Thread::current(), "Expected thread parameter to be current thread.");
  if (ShenandoahThreadLocalData::is_oom_during_evac(thread)) {
    // This thread went through the OOM during evac protocol and it is safe to return
    // the forward pointer. It must not attempt to evacuate anymore.
    return ShenandoahBarrierSet::resolve_forwarded(p);
  }

  assert(ShenandoahThreadLocalData::is_evac_allowed(thread), "must be enclosed in oom-evac scope");

  ShenandoahHeapRegion* r = heap_region_containing(p);
  assert(!r->is_humongous(), "never evacuate humongous objects");

  ShenandoahAffiliation target_gen = r->affiliation();
  if (active_generation()->is_young() && target_gen == YOUNG_GENERATION) {
    markWord mark = p->mark();
    if (mark.is_marked()) {
      // Already forwarded.
      return ShenandoahBarrierSet::resolve_forwarded(p);
    }

    if (mark.has_displaced_mark_helper()) {
      // We don't want to deal with MT here just to ensure we read the right mark word.
      // Skip the potential promotion attempt for this one.
    } else if (r->age() + mark.age() >= age_census()->tenuring_threshold()) {
      oop result = try_evacuate_object(p, thread, r, OLD_GENERATION);
      if (result != nullptr) {
        return result;
      }
      // If we failed to promote this aged object, we'll fall through to code below and evacuate to young-gen.
    }
  }
  return try_evacuate_object(p, thread, r, target_gen);
}

// try_evacuate_object registers the object and dirties the associated remembered set information when evacuating
// to OLD_GENERATION.
oop ShenandoahGenerationalHeap::try_evacuate_object(oop p, Thread* thread, ShenandoahHeapRegion* from_region,
                                        ShenandoahAffiliation target_gen) {
  bool alloc_from_lab = true;
  bool has_plab = false;
  HeapWord* copy = nullptr;
  size_t size = p->size();
  bool is_promotion = (target_gen == OLD_GENERATION) && from_region->is_young();

#ifdef ASSERT
  if (ShenandoahOOMDuringEvacALot &&
      (os::random() & 1) == 0) { // Simulate OOM every ~2nd slow-path call
    copy = nullptr;
  } else {
#endif
    if (UseTLAB) {
      switch (target_gen) {
        case YOUNG_GENERATION: {
          copy = allocate_from_gclab(thread, size);
          if ((copy == nullptr) && (size < ShenandoahThreadLocalData::gclab_size(thread))) {
            // GCLAB allocation failed because we are bumping up against the limit on young evacuation reserve.  Try resetting
            // the desired GCLAB size and retry GCLAB allocation to avoid cascading of shared memory allocations.
            ShenandoahThreadLocalData::set_gclab_size(thread, PLAB::min_size());
            copy = allocate_from_gclab(thread, size);
            // If we still get nullptr, we'll try a shared allocation below.
          }
          break;
        }
        case OLD_GENERATION: {
          assert(mode()->is_generational(), "OLD Generation only exists in generational mode");
          PLAB* plab = ShenandoahThreadLocalData::plab(thread);
          if (plab != nullptr) {
            has_plab = true;
          }
          copy = allocate_from_plab(thread, size, is_promotion);
          if ((copy == nullptr) && (size < ShenandoahThreadLocalData::plab_size(thread)) &&
              ShenandoahThreadLocalData::plab_retries_enabled(thread)) {
            // PLAB allocation failed because we are bumping up against the limit on old evacuation reserve or because
            // the requested object does not fit within the current plab but the plab still has an "abundance" of memory,
            // where abundance is defined as >= ShenGenHeap::plab_min_size().  In the former case, we try resetting the desired
            // PLAB size and retry PLAB allocation to avoid cascading of shared memory allocations.

            // In this situation, PLAB memory is precious.  We'll try to preserve our existing PLAB by forcing
            // this particular allocation to be shared.
            if (plab->words_remaining() < plab_min_size()) {
              ShenandoahThreadLocalData::set_plab_size(thread, plab_min_size());
              copy = allocate_from_plab(thread, size, is_promotion);
              // If we still get nullptr, we'll try a shared allocation below.
              if (copy == nullptr) {
                // If retry fails, don't continue to retry until we have success (probably in next GC pass)
                ShenandoahThreadLocalData::disable_plab_retries(thread);
              }
            }
            // else, copy still equals nullptr.  this causes shared allocation below, preserving this plab for future needs.
          }
          break;
        }
        default: {
          ShouldNotReachHere();
          break;
        }
      }
    }

    if (copy == nullptr) {
      // If we failed to allocate in LAB, we'll try a shared allocation.
      if (!is_promotion || !has_plab || (size > PLAB::min_size())) {
        ShenandoahAllocRequest req = ShenandoahAllocRequest::for_shared_gc(size, target_gen, is_promotion);
        copy = allocate_memory(req);
        alloc_from_lab = false;
      }
      // else, we leave copy equal to nullptr, signaling a promotion failure below if appropriate.
      // We choose not to promote objects smaller than PLAB::min_size() by way of shared allocations, as this is too
      // costly.  Instead, we'll simply "evacuate" to young-gen memory (using a GCLAB) and will promote in a future
      // evacuation pass.  This condition is denoted by: is_promotion && has_plab && (size <= PLAB::min_size())
    }
#ifdef ASSERT
  }
#endif

  if (copy == nullptr) {
    if (target_gen == OLD_GENERATION) {
      if (from_region->is_young()) {
        // Signal that promotion failed. Will evacuate this old object somewhere in young gen.
        old_generation()->handle_failed_promotion(thread, size);
        return nullptr;
      } else {
        // Remember that evacuation to old gen failed. We'll want to trigger a full gc to recover from this
        // after the evacuation threads have finished.
        old_generation()->handle_failed_evacuation();
      }
    }

    control_thread()->handle_alloc_failure_evac(size);

    oom_evac_handler()->handle_out_of_memory_during_evacuation();

    return ShenandoahBarrierSet::resolve_forwarded(p);
  }

  // Copy the object:
  evac_tracker()->begin_evacuation(thread, size * HeapWordSize);
  Copy::aligned_disjoint_words(cast_from_oop<HeapWord*>(p), copy, size);

  oop copy_val = cast_to_oop(copy);

  if (target_gen == YOUNG_GENERATION && is_aging_cycle()) {
    ShenandoahHeap::increase_object_age(copy_val, from_region->age() + 1);
  }

  // Try to install the new forwarding pointer.
  ContinuationGCSupport::relativize_stack_chunk(copy_val);

  oop result = ShenandoahForwarding::try_update_forwardee(p, copy_val);
  if (result == copy_val) {
    // Successfully evacuated. Our copy is now the public one!
    evac_tracker()->end_evacuation(thread, size * HeapWordSize);
    if (target_gen == OLD_GENERATION) {
      old_generation()->handle_evacuation(copy, size, from_region->is_young());
    } else {
      // When copying to the old generation above, we don't care
      // about recording object age in the census stats.
      assert(target_gen == YOUNG_GENERATION, "Error");
      // We record this census only when simulating pre-adaptive tenuring behavior, or
      // when we have been asked to record the census at evacuation rather than at mark
      if (ShenandoahGenerationalCensusAtEvac || !ShenandoahGenerationalAdaptiveTenuring) {
        evac_tracker()->record_age(thread, size * HeapWordSize, ShenandoahHeap::get_object_age(copy_val));
      }
    }
    shenandoah_assert_correct(nullptr, copy_val);
    return copy_val;
  }  else {
    // Failed to evacuate. We need to deal with the object that is left behind. Since this
    // new allocation is certainly after TAMS, it will be considered live in the next cycle.
    // But if it happens to contain references to evacuated regions, those references would
    // not get updated for this stale copy during this cycle, and we will crash while scanning
    // it the next cycle.
    if (alloc_from_lab) {
      // For LAB allocations, it is enough to rollback the allocation ptr. Either the next
      // object will overwrite this stale copy, or the filler object on LAB retirement will
      // do this.
      switch (target_gen) {
        case YOUNG_GENERATION: {
          ShenandoahThreadLocalData::gclab(thread)->undo_allocation(copy, size);
          break;
        }
        case OLD_GENERATION: {
          ShenandoahThreadLocalData::plab(thread)->undo_allocation(copy, size);
          if (is_promotion) {
            ShenandoahThreadLocalData::subtract_from_plab_promoted(thread, size * HeapWordSize);
          }
          break;
        }
        default: {
          ShouldNotReachHere();
          break;
        }
      }
    } else {
      // For non-LAB allocations, we have no way to retract the allocation, and
      // have to explicitly overwrite the copy with the filler object. With that overwrite,
      // we have to keep the fwdptr initialized and pointing to our (stale) copy.
      assert(size >= ShenandoahHeap::min_fill_size(), "previously allocated object known to be larger than min_size");
      fill_with_object(copy, size);
      shenandoah_assert_correct(nullptr, copy_val);
      // For non-LAB allocations, the object has already been registered
    }
    shenandoah_assert_correct(nullptr, result);
    return result;
  }
}

inline HeapWord* ShenandoahGenerationalHeap::allocate_from_plab(Thread* thread, size_t size, bool is_promotion) {
  assert(UseTLAB, "TLABs should be enabled");

  PLAB* plab = ShenandoahThreadLocalData::plab(thread);
  HeapWord* obj;

  if (plab == nullptr) {
    assert(!thread->is_Java_thread() && !thread->is_Worker_thread(), "Performance: thread should have PLAB: %s", thread->name());
    // No PLABs in this thread, fallback to shared allocation
    return nullptr;
  } else if (is_promotion && !ShenandoahThreadLocalData::allow_plab_promotions(thread)) {
    return nullptr;
  }
  // if plab->word_size() <= 0, thread's plab not yet initialized for this pass, so allow_plab_promotions() is not trustworthy
  obj = plab->allocate(size);
  if ((obj == nullptr) && (plab->words_remaining() < plab_min_size())) {
    // allocate_from_plab_slow will establish allow_plab_promotions(thread) for future invocations
    obj = allocate_from_plab_slow(thread, size, is_promotion);
  }
  // if plab->words_remaining() >= ShenGenHeap::heap()->plab_min_size(), just return nullptr so we can use a shared allocation
  if (obj == nullptr) {
    return nullptr;
  }

  if (is_promotion) {
    ShenandoahThreadLocalData::add_to_plab_promoted(thread, size * HeapWordSize);
  }
  return obj;
}

// Establish a new PLAB and allocate size HeapWords within it.
HeapWord* ShenandoahGenerationalHeap::allocate_from_plab_slow(Thread* thread, size_t size, bool is_promotion) {
  // New object should fit the PLAB size

  assert(mode()->is_generational(), "PLABs only relevant to generational GC");
  const size_t plab_min_size = this->plab_min_size();
  const size_t min_size = (size > plab_min_size)? align_up(size, CardTable::card_size_in_words()): plab_min_size;

  // Figure out size of new PLAB, looking back at heuristics. Expand aggressively.  PLABs must align on size
  // of card table in order to avoid the need for synchronization when registering newly allocated objects within
  // the card table.
  size_t cur_size = ShenandoahThreadLocalData::plab_size(thread);
  if (cur_size == 0) {
    cur_size = plab_min_size;
  }

  // Limit growth of PLABs to the smaller of ShenandoahMaxEvacLABRatio * the minimum size and ShenandoahHumongousThreshold.
  // This minimum value is represented by generational_heap->plab_max_size().  Enforcing this limit enables more equitable
  // distribution of available evacuation budget between the many threads that are coordinating in the evacuation effort.
  size_t future_size = MIN2(cur_size * 2, plab_max_size());
  assert(is_aligned(future_size, CardTable::card_size_in_words()), "Align by design, future_size: " SIZE_FORMAT
          ", alignment: " SIZE_FORMAT ", cur_size: " SIZE_FORMAT ", max: " SIZE_FORMAT,
         future_size, (size_t) CardTable::card_size_in_words(), cur_size, plab_max_size());

  // Record new heuristic value even if we take any shortcut. This captures
  // the case when moderately-sized objects always take a shortcut. At some point,
  // heuristics should catch up with them.  Note that the requested cur_size may
  // not be honored, but we remember that this is the preferred size.
  ShenandoahThreadLocalData::set_plab_size(thread, future_size);
  if (cur_size < size) {
    // The PLAB to be allocated is still not large enough to hold the object. Fall back to shared allocation.
    // This avoids retiring perfectly good PLABs in order to represent a single large object allocation.
    return nullptr;
  }

  // Retire current PLAB, and allocate a new one.
  PLAB* plab = ShenandoahThreadLocalData::plab(thread);
  if (plab->words_remaining() < plab_min_size) {
    // Retire current PLAB, and allocate a new one.
    // CAUTION: retire_plab may register the remnant filler object with the remembered set scanner without a lock.  This
    // is safe iff it is assured that each PLAB is a whole-number multiple of card-mark memory size and each PLAB is
    // aligned with the start of a card's memory range.
    retire_plab(plab, thread);

    size_t actual_size = 0;
    // allocate_new_plab resets plab_evacuated and plab_promoted and disables promotions if old-gen available is
    // less than the remaining evacuation need.  It also adjusts plab_preallocated and expend_promoted if appropriate.
    HeapWord* plab_buf = allocate_new_plab(min_size, cur_size, &actual_size);
    if (plab_buf == nullptr) {
      if (min_size == plab_min_size) {
        // Disable plab promotions for this thread because we cannot even allocate a plab of minimal size.  This allows us
        // to fail faster on subsequent promotion attempts.
        ShenandoahThreadLocalData::disable_plab_promotions(thread);
      }
      return nullptr;
    } else {
      ShenandoahThreadLocalData::enable_plab_retries(thread);
    }
    // Since the allocated PLAB may have been down-sized for alignment, plab->allocate(size) below may still fail.
    if (ZeroTLAB) {
      // ... and clear it.
      Copy::zero_to_words(plab_buf, actual_size);
    } else {
      // ...and zap just allocated object.
#ifdef ASSERT
      // Skip mangling the space corresponding to the object header to
      // ensure that the returned space is not considered parsable by
      // any concurrent GC thread.
      size_t hdr_size = oopDesc::header_size();
      Copy::fill_to_words(plab_buf + hdr_size, actual_size - hdr_size, badHeapWordVal);
#endif // ASSERT
    }
    assert(is_aligned(actual_size, CardTable::card_size_in_words()), "Align by design");
    plab->set_buf(plab_buf, actual_size);
    if (is_promotion && !ShenandoahThreadLocalData::allow_plab_promotions(thread)) {
      return nullptr;
    }
    return plab->allocate(size);
  } else {
    // If there's still at least min_size() words available within the current plab, don't retire it.  Let's gnaw
    // away on this plab as long as we can.  Meanwhile, return nullptr to force this particular allocation request
    // to be satisfied with a shared allocation.  By packing more promotions into the previously allocated PLAB, we
    // reduce the likelihood of evacuation failures, and we reduce the need for downsizing our PLABs.
    return nullptr;
  }
}

HeapWord* ShenandoahGenerationalHeap::allocate_new_plab(size_t min_size, size_t word_size, size_t* actual_size) {
  // Align requested sizes to card-sized multiples.  Align down so that we don't violate max size of TLAB.
  assert(is_aligned(min_size, CardTable::card_size_in_words()), "Align by design");
  assert(word_size >= min_size, "Requested PLAB is too small");

  ShenandoahAllocRequest req = ShenandoahAllocRequest::for_plab(min_size, word_size);
  // Note that allocate_memory() sets a thread-local flag to prohibit further promotions by this thread
  // if we are at risk of infringing on the old-gen evacuation budget.
  HeapWord* res = allocate_memory(req);
  if (res != nullptr) {
    *actual_size = req.actual_size();
  } else {
    *actual_size = 0;
  }
  assert(is_aligned(res, CardTable::card_size_in_words()), "Align by design");
  return res;
}

// TODO: It is probably most efficient to register all objects (both promotions and evacuations) that were allocated within
// this plab at the time we retire the plab.  A tight registration loop will run within both code and data caches.  This change
// would allow smaller and faster in-line implementation of alloc_from_plab().  Since plabs are aligned on card-table boundaries,
// this object registration loop can be performed without acquiring a lock.
void ShenandoahGenerationalHeap::retire_plab(PLAB* plab, Thread* thread) {
  // We don't enforce limits on plab evacuations.  We let it consume all available old-gen memory in order to reduce
  // probability of an evacuation failure.  We do enforce limits on promotion, to make sure that excessive promotion
  // does not result in an old-gen evacuation failure.  Note that a failed promotion is relatively harmless.  Any
  // object that fails to promote in the current cycle will be eligible for promotion in a subsequent cycle.

  // When the plab was instantiated, its entirety was treated as if the entire buffer was going to be dedicated to
  // promotions.  Now that we are retiring the buffer, we adjust for the reality that the plab is not entirely promotions.
  //  1. Some of the plab may have been dedicated to evacuations.
  //  2. Some of the plab may have been abandoned due to waste (at the end of the plab).
  size_t not_promoted =
          ShenandoahThreadLocalData::get_plab_actual_size(thread) - ShenandoahThreadLocalData::get_plab_promoted(thread);
  ShenandoahThreadLocalData::reset_plab_promoted(thread);
  ShenandoahThreadLocalData::set_plab_actual_size(thread, 0);
  if (not_promoted > 0) {
    old_generation()->unexpend_promoted(not_promoted);
  }
  const size_t original_waste = plab->waste();
  HeapWord* const top = plab->top();

  // plab->retire() overwrites unused memory between plab->top() and plab->hard_end() with a dummy object to make memory parsable.
  // It adds the size of this unused memory, in words, to plab->waste().
  plab->retire();
  if (top != nullptr && plab->waste() > original_waste && is_in_old(top)) {
    // If retiring the plab created a filler object, then we need to register it with our card scanner so it can
    // safely walk the region backing the plab.
    log_debug(gc)("retire_plab() is registering remnant of size " SIZE_FORMAT " at " PTR_FORMAT,
                  plab->waste() - original_waste, p2i(top));
    card_scan()->register_object_without_lock(top);
  }
}

void ShenandoahGenerationalHeap::retire_plab(PLAB* plab) {
  Thread* thread = Thread::current();
  retire_plab(plab, thread);
}

ShenandoahGenerationalHeap::TransferResult ShenandoahGenerationalHeap::balance_generations() {
  shenandoah_assert_heaplocked_or_safepoint();

  ShenandoahOldGeneration* old_gen = old_generation();
  const ssize_t old_region_balance = old_gen->get_region_balance();
  old_gen->set_region_balance(0);

  if (old_region_balance > 0) {
    const auto old_region_surplus = checked_cast<size_t>(old_region_balance);
    const bool success = generation_sizer()->transfer_to_young(old_region_surplus);
    return TransferResult {
      success, old_region_surplus, "young"
    };
  }

  if (old_region_balance < 0) {
    const auto old_region_deficit = checked_cast<size_t>(-old_region_balance);
    const bool success = generation_sizer()->transfer_to_old(old_region_deficit);
    if (!success) {
      old_gen->handle_failed_transfer();
    }
    return TransferResult {
      success, old_region_deficit, "old"
    };
  }

  return TransferResult {true, 0, "none"};
}

// Make sure old-generation is large enough, but no larger than is necessary, to hold mixed evacuations
// and promotions, if we anticipate either. Any deficit is provided by the young generation, subject to
// xfer_limit, and any surplus is transferred to the young generation.
// xfer_limit is the maximum we're able to transfer from young to old.
void ShenandoahGenerationalHeap::compute_old_generation_balance(size_t old_xfer_limit, size_t old_cset_regions) {

  // We can limit the old reserve to the size of anticipated promotions:
  // max_old_reserve is an upper bound on memory evacuated from old and promoted to old,
  // clamped by the old generation space available.
  //
  // Here's the algebra.
  // Let SOEP = ShenandoahOldEvacRatioPercent,
  //     OE = old evac,
  //     YE = young evac, and
  //     TE = total evac = OE + YE
  // By definition:
  //            SOEP/100 = OE/TE
  //                     = OE/(OE+YE)
  //  => SOEP/(100-SOEP) = OE/((OE+YE)-OE)      // componendo-dividendo: If a/b = c/d, then a/(b-a) = c/(d-c)
  //                     = OE/YE
  //  =>              OE = YE*SOEP/(100-SOEP)

  // We have to be careful in the event that SOEP is set to 100 by the user.
  assert(ShenandoahOldEvacRatioPercent <= 100, "Error");
  const size_t old_available = old_generation()->available();
  // The free set will reserve this amount of memory to hold young evacuations
  const size_t young_reserve = (young_generation()->max_capacity() * ShenandoahEvacReserve) / 100;

  // In the case that ShenandoahOldEvacRatioPercent equals 100, max_old_reserve is limited only by xfer_limit.

  const size_t bound_on_old_reserve = old_available + old_xfer_limit + young_reserve;
  const size_t max_old_reserve = (ShenandoahOldEvacRatioPercent == 100)?
                                 bound_on_old_reserve: MIN2((young_reserve * ShenandoahOldEvacRatioPercent) / (100 - ShenandoahOldEvacRatioPercent),
                                                            bound_on_old_reserve);

  const size_t region_size_bytes = ShenandoahHeapRegion::region_size_bytes();

  // Decide how much old space we should reserve for a mixed collection
  size_t reserve_for_mixed = 0;
  if (old_generation()->has_unprocessed_collection_candidates()) {
    // We want this much memory to be unfragmented in order to reliably evacuate old.  This is conservative because we
    // may not evacuate the entirety of unprocessed candidates in a single mixed evacuation.
    const size_t max_evac_need = (size_t)
            (old_generation()->unprocessed_collection_candidates_live_memory() * ShenandoahOldEvacWaste);
    assert(old_available >= old_generation()->free_unaffiliated_regions() * region_size_bytes,
           "Unaffiliated available must be less than total available");
    const size_t old_fragmented_available =
            old_available - old_generation()->free_unaffiliated_regions() * region_size_bytes;
    reserve_for_mixed = max_evac_need + old_fragmented_available;
    if (reserve_for_mixed > max_old_reserve) {
      reserve_for_mixed = max_old_reserve;
    }
  }

  // Decide how much space we should reserve for promotions from young
  size_t reserve_for_promo = 0;
  const size_t promo_load = old_generation()->get_promotion_potential();
  const bool doing_promotions = promo_load > 0;
  if (doing_promotions) {
    // We're promoting and have a bound on the maximum amount that can be promoted
    assert(max_old_reserve >= reserve_for_mixed, "Sanity");
    const size_t available_for_promotions = max_old_reserve - reserve_for_mixed;
    reserve_for_promo = MIN2((size_t)(promo_load * ShenandoahPromoEvacWaste), available_for_promotions);
  }

  // This is the total old we want to ideally reserve
  const size_t old_reserve = reserve_for_mixed + reserve_for_promo;
  assert(old_reserve <= max_old_reserve, "cannot reserve more than max for old evacuations");

  // We now check if the old generation is running a surplus or a deficit.
  const size_t max_old_available = old_generation()->available() + old_cset_regions * region_size_bytes;
  if (max_old_available >= old_reserve) {
    // We are running a surplus, so the old region surplus can go to young
    const size_t old_surplus = (max_old_available - old_reserve) / region_size_bytes;
    const size_t unaffiliated_old_regions = old_generation()->free_unaffiliated_regions() + old_cset_regions;
    const size_t old_region_surplus = MIN2(old_surplus, unaffiliated_old_regions);
    old_generation()->set_region_balance(checked_cast<ssize_t>(old_region_surplus));
  } else {
    // We are running a deficit which we'd like to fill from young.
    // Ignore that this will directly impact young_generation()->max_capacity(),
    // indirectly impacting young_reserve and old_reserve.  These computations are conservative.
    // Note that deficit is rounded up by one region.
    const size_t old_need = (old_reserve - max_old_available + region_size_bytes - 1) / region_size_bytes;
    const size_t max_old_region_xfer = old_xfer_limit / region_size_bytes;

    // Round down the regions we can transfer from young to old. If we're running short
    // on young-gen memory, we restrict the xfer. Old-gen collection activities will be
    // curtailed if the budget is restricted.
    const size_t old_region_deficit = MIN2(old_need, max_old_region_xfer);
    old_generation()->set_region_balance(0 - checked_cast<ssize_t>(old_region_deficit));
  }
}

void ShenandoahGenerationalHeap::reset_generation_reserves() {
  young_generation()->set_evacuation_reserve(0);
  old_generation()->set_evacuation_reserve(0);
  old_generation()->set_promoted_reserve(0);
}

void ShenandoahGenerationalHeap::TransferResult::print_on(const char* when, outputStream* ss) const {
  auto heap = ShenandoahGenerationalHeap::heap();
  ShenandoahYoungGeneration* const young_gen = heap->young_generation();
  ShenandoahOldGeneration* const old_gen = heap->old_generation();
  const size_t young_available = young_gen->available();
  const size_t old_available = old_gen->available();
  ss->print_cr("After %s, %s " SIZE_FORMAT " regions to %s to prepare for next gc, old available: "
                     PROPERFMT ", young_available: " PROPERFMT,
                     when,
                     success? "successfully transferred": "failed to transfer", region_count, region_destination,
                     PROPERFMTARGS(old_available), PROPERFMTARGS(young_available));
}

void ShenandoahGenerationalHeap::coalesce_and_fill_old_regions(bool concurrent) {
  class ShenandoahGlobalCoalesceAndFill : public WorkerTask {
  private:
      ShenandoahPhaseTimings::Phase _phase;
      ShenandoahRegionIterator _regions;
  public:
    explicit ShenandoahGlobalCoalesceAndFill(ShenandoahPhaseTimings::Phase phase) :
      WorkerTask("Shenandoah Global Coalesce"),
      _phase(phase) {}

    void work(uint worker_id) override {
      ShenandoahWorkerTimingsTracker timer(_phase,
                                           ShenandoahPhaseTimings::ScanClusters,
                                           worker_id, true);
      ShenandoahHeapRegion* region;
      while ((region = _regions.next()) != nullptr) {
        // old region is not in the collection set and was not immediately trashed
        if (region->is_old() && region->is_active() && !region->is_humongous()) {
          // Reset the coalesce and fill boundary because this is a global collect
          // and cannot be preempted by young collects. We want to be sure the entire
          // region is coalesced here and does not resume from a previously interrupted
          // or completed coalescing.
          region->begin_preemptible_coalesce_and_fill();
          region->oop_coalesce_and_fill(false);
        }
      }
    }
  };

  ShenandoahPhaseTimings::Phase phase = concurrent ?
          ShenandoahPhaseTimings::conc_coalesce_and_fill :
          ShenandoahPhaseTimings::degen_gc_coalesce_and_fill;

  // This is not cancellable
  ShenandoahGlobalCoalesceAndFill coalesce(phase);
  workers()->run_task(&coalesce);
  old_generation()->set_parseable(true);
}

template<bool CONCURRENT>
class ShenandoahGenerationalUpdateHeapRefsTask : public WorkerTask {
private:
  ShenandoahHeap* _heap;
  ShenandoahRegionIterator* _regions;
  ShenandoahRegionChunkIterator* _work_chunks;

public:
  explicit ShenandoahGenerationalUpdateHeapRefsTask(ShenandoahRegionIterator* regions,
                                                    ShenandoahRegionChunkIterator* work_chunks) :
          WorkerTask("Shenandoah Update References"),
          _heap(ShenandoahHeap::heap()),
          _regions(regions),
          _work_chunks(work_chunks)
  {
    bool old_bitmap_stable = _heap->old_generation()->is_mark_complete();
    log_debug(gc, remset)("Update refs, scan remembered set using bitmap: %s", BOOL_TO_STR(old_bitmap_stable));
  }

  void work(uint worker_id) {
    if (CONCURRENT) {
      ShenandoahConcurrentWorkerSession worker_session(worker_id);
      ShenandoahSuspendibleThreadSetJoiner stsj;
      do_work<ShenandoahConcUpdateRefsClosure>(worker_id);
    } else {
      ShenandoahParallelWorkerSession worker_session(worker_id);
      do_work<ShenandoahSTWUpdateRefsClosure>(worker_id);
    }
  }

private:
  template<class T>
  void do_work(uint worker_id) {
    T cl;
    if (CONCURRENT && (worker_id == 0)) {
      // We ask the first worker to replenish the Mutator free set by moving regions previously reserved to hold the
      // results of evacuation.  These reserves are no longer necessary because evacuation has completed.
      size_t cset_regions = _heap->collection_set()->count();
      // We cannot transfer any more regions than will be reclaimed when the existing collection set is recycled, because
      // we need the reclaimed collection set regions to replenish the collector reserves
      _heap->free_set()->move_collector_sets_to_mutator(cset_regions);
    }
    // If !CONCURRENT, there's no value in expanding Mutator free set

    ShenandoahHeapRegion* r = _regions->next();
    // We update references for global, old, and young collections.
    assert(_heap->active_generation()->is_mark_complete(), "Expected complete marking");
    ShenandoahMarkingContext* const ctx = _heap->marking_context();
    bool is_mixed = _heap->collection_set()->has_old_regions();
    while (r != nullptr) {
      HeapWord* update_watermark = r->get_update_watermark();
      assert (update_watermark >= r->bottom(), "sanity");

      log_debug(gc)("Update refs worker " UINT32_FORMAT ", looking at region " SIZE_FORMAT, worker_id, r->index());
      bool region_progress = false;
      if (r->is_active() && !r->is_cset()) {
        if (r->is_young()) {
          _heap->marked_object_oop_iterate(r, &cl, update_watermark);
          region_progress = true;
        } else if (r->is_old()) {
          if (_heap->active_generation()->is_global()) {
            // Note that GLOBAL collection is not as effectively balanced as young and mixed cycles.  This is because
            // concurrent GC threads are parceled out entire heap regions of work at a time and there
            // is no "catchup phase" consisting of remembered set scanning, during which parcels of work are smaller
            // and more easily distributed more fairly across threads.

            // TODO: Consider an improvement to load balance GLOBAL GC.
            _heap->marked_object_oop_iterate(r, &cl, update_watermark);
            region_progress = true;
          }
          // Otherwise, this is an old region in a young or mixed cycle.  Process it during a second phase, below.
          // Don't bother to report pacing progress in this case.
        } else {
          // Because updating of references runs concurrently, it is possible that a FREE inactive region transitions
          // to a non-free active region while this loop is executing.  Whenever this happens, the changing of a region's
          // active status may propagate at a different speed than the changing of the region's affiliation.

          // When we reach this control point, it is because a race has allowed a region's is_active() status to be seen
          // by this thread before the region's affiliation() is seen by this thread.

          // It's ok for this race to occur because the newly transformed region does not have any references to be
          // updated.

          assert(r->get_update_watermark() == r->bottom(),
                 "%s Region " SIZE_FORMAT " is_active but not recognized as YOUNG or OLD so must be newly transitioned from FREE",
                 r->affiliation_name(), r->index());
        }
      }
      if (region_progress && ShenandoahPacing) {
        _heap->pacer()->report_updaterefs(pointer_delta(update_watermark, r->bottom()));
      }
      if (_heap->check_cancelled_gc_and_yield(CONCURRENT)) {
        return;
      }
      r = _regions->next();
    }

    if (!_heap->active_generation()->is_global()) {
      // Since this is generational and not GLOBAL, we have to process the remembered set.  There's no remembered
      // set processing if not in generational mode or if GLOBAL mode.

      // After this thread has exhausted its traditional update-refs work, it continues with updating refs within remembered set.
      // The remembered set workload is better balanced between threads, so threads that are "behind" can catch up with other
      // threads during this phase, allowing all threads to work more effectively in parallel.
      struct ShenandoahRegionChunk assignment;
      RememberedScanner* scanner = _heap->card_scan();

      while (!_heap->check_cancelled_gc_and_yield(CONCURRENT) && _work_chunks->next(&assignment)) {
        // Keep grabbing next work chunk to process until finished, or asked to yield
        ShenandoahHeapRegion* r = assignment._r;
        if (r->is_active() && !r->is_cset() && r->is_old()) {
          HeapWord* start_of_range = r->bottom() + assignment._chunk_offset;
          HeapWord* end_of_range = r->get_update_watermark();
          if (end_of_range > start_of_range + assignment._chunk_size) {
            end_of_range = start_of_range + assignment._chunk_size;
          }

          // Old region in a young cycle or mixed cycle.
          if (is_mixed) {
            // TODO: For mixed evac, consider building an old-gen remembered set that allows restricted updating
            // within old-gen HeapRegions.  This remembered set can be constructed by old-gen concurrent marking
            // and augmented by card marking.  For example, old-gen concurrent marking can remember for each old-gen
            // card which other old-gen regions it refers to: none, one-other specifically, multiple-other non-specific.
            // Update-references when _mixed_evac processess each old-gen memory range that has a traditional DIRTY
            // card or if the "old-gen remembered set" indicates that this card holds pointers specifically to an
            // old-gen region in the most recent collection set, or if this card holds pointers to other non-specific
            // old-gen heap regions.

            if (r->is_humongous()) {
              if (start_of_range < end_of_range) {
                // Need to examine both dirty and clean cards during mixed evac.
                r->oop_iterate_humongous_slice(&cl, false, start_of_range, assignment._chunk_size, true);
              }
            } else {
              // Since this is mixed evacuation, old regions that are candidates for collection have not been coalesced
              // and filled.  Use mark bits to find objects that need to be updated.
              //
              // Future TODO: establish a second remembered set to identify which old-gen regions point to other old-gen
              // regions which are in the collection set for a particular mixed evacuation.
              if (start_of_range < end_of_range) {
                HeapWord* p = nullptr;
                size_t card_index = scanner->card_index_for_addr(start_of_range);
                // In case last object in my range spans boundary of my chunk, I may need to scan all the way to top()
                ShenandoahObjectToOopBoundedClosure<T> objs(&cl, start_of_range, r->top());

                // Any object that begins in a previous range is part of a different scanning assignment.  Any object that
                // starts after end_of_range is also not my responsibility.  (Either allocated during evacuation, so does
                // not hold pointers to from-space, or is beyond the range of my assigned work chunk.)

                // Find the first object that begins in my range, if there is one.
                p = start_of_range;
                oop obj = cast_to_oop(p);
                HeapWord* tams = ctx->top_at_mark_start(r);
                if (p >= tams) {
                  // We cannot use ctx->is_marked(obj) to test whether an object begins at this address.  Instead,
                  // we need to use the remembered set crossing map to advance p to the first object that starts
                  // within the enclosing card.

                  while (true) {
                    HeapWord* first_object = scanner->first_object_in_card(card_index);
                    if (first_object != nullptr) {
                      p = first_object;
                      break;
                    } else if (scanner->addr_for_card_index(card_index + 1) < end_of_range) {
                      card_index++;
                    } else {
                      // Force the loop that follows to immediately terminate.
                      p = end_of_range;
                      break;
                    }
                  }
                  obj = cast_to_oop(p);
                  // Note: p may be >= end_of_range
                } else if (!ctx->is_marked(obj)) {
                  p = ctx->get_next_marked_addr(p, tams);
                  obj = cast_to_oop(p);
                  // If there are no more marked objects before tams, this returns tams.
                  // Note that tams is either >= end_of_range, or tams is the start of an object that is marked.
                }
                while (p < end_of_range) {
                  // p is known to point to the beginning of marked object obj
                  objs.do_object(obj);
                  HeapWord* prev_p = p;
                  p += obj->size();
                  if (p < tams) {
                    p = ctx->get_next_marked_addr(p, tams);
                    // If there are no more marked objects before tams, this returns tams.  Note that tams is
                    // either >= end_of_range, or tams is the start of an object that is marked.
                  }
                  assert(p != prev_p, "Lack of forward progress");
                  obj = cast_to_oop(p);
                }
              }
            }
          } else {
            // This is a young evac..
            if (start_of_range < end_of_range) {
              size_t cluster_size =
                      CardTable::card_size_in_words() * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
              size_t clusters = assignment._chunk_size / cluster_size;
              assert(clusters * cluster_size == assignment._chunk_size, "Chunk assignment must align on cluster boundaries");
              scanner->process_region_slice(r, assignment._chunk_offset, clusters, end_of_range, &cl, true, worker_id);
            }
          }
          if (ShenandoahPacing && (start_of_range < end_of_range)) {
            _heap->pacer()->report_updaterefs(pointer_delta(end_of_range, start_of_range));
          }
        }
      }
    }
  }
};

void ShenandoahGenerationalHeap::update_heap_references(bool concurrent) {
  assert(!is_full_gc_in_progress(), "Only for concurrent and degenerated GC");
  uint nworkers = workers()->active_workers();
  ShenandoahRegionChunkIterator work_list(nworkers);
  ShenandoahRegionIterator update_refs_iterator(this);
  if (concurrent) {
    ShenandoahGenerationalUpdateHeapRefsTask<true> task(&update_refs_iterator, &work_list);
    workers()->run_task(&task);
  } else {
    ShenandoahGenerationalUpdateHeapRefsTask<false> task(&update_refs_iterator, &work_list);
    workers()->run_task(&task);
  }
  assert(cancelled_gc() || !update_refs_iterator.has_next(), "Should have finished update references");

  if (ShenandoahEnableCardStats) { // generational check proxy
    assert(card_scan() != nullptr, "Card table must exist when card stats are enabled");
    card_scan()->log_card_stats(nworkers, CARD_STAT_UPDATE_REFS);
  }
}

void ShenandoahGenerationalHeap::complete_degenerated_cycle() {
  shenandoah_assert_heaplocked_or_safepoint();
  if (is_concurrent_old_mark_in_progress()) {
    // This is still necessary for degenerated cycles because the degeneration point may occur
    // after final mark of the young generation. See ShenandoahConcurrentGC::op_final_updaterefs for
    // a more detailed explanation.
    old_generation()->transfer_pointers_from_satb();
  }

  // We defer generation resizing actions until after cset regions have been recycled.
  TransferResult result = balance_generations();
  LogTarget(Info, gc, ergo) lt;
  if (lt.is_enabled()) {
    LogStream ls(lt);
    result.print_on("Degenerated GC", &ls);
  }

  // In case degeneration interrupted concurrent evacuation or update references, we need to clean up
  // transient state. Otherwise, these actions have no effect.
  reset_generation_reserves();

  if (!old_generation()->is_parseable()) {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::degen_gc_coalesce_and_fill);
    coalesce_and_fill_old_regions(false);
  }
}

void ShenandoahGenerationalHeap::complete_concurrent_cycle() {
  if (!old_generation()->is_parseable()) {
    // Class unloading may render the card offsets unusable, so we must rebuild them before
    // the next remembered set scan. We _could_ let the control thread do this sometime after
    // the global cycle has completed and before the next young collection, but under memory
    // pressure the control thread may not have the time (that is, because it's running back
    // to back GCs). In that scenario, we would have to make the old regions parsable before
    // we could start a young collection. This could delay the start of the young cycle and
    // throw off the heuristics.
    entry_global_coalesce_and_fill();
  }

  TransferResult result;
  {
    ShenandoahHeapLocker locker(lock());

    result = balance_generations();
    reset_generation_reserves();
  }

  LogTarget(Info, gc, ergo) lt;
  if (lt.is_enabled()) {
    LogStream ls(lt);
    result.print_on("Concurrent GC", &ls);
  }
}

void ShenandoahGenerationalHeap::entry_global_coalesce_and_fill() {
  const char* msg = "Coalescing and filling old regions";
  ShenandoahConcurrentPhase gc_phase(msg, ShenandoahPhaseTimings::conc_coalesce_and_fill);

  TraceCollectorStats tcs(monitoring_support()->concurrent_collection_counters());
  EventMark em("%s", msg);
  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_marking(),
                              "concurrent coalesce and fill");

  coalesce_and_fill_old_regions(true);
}

void ShenandoahGenerationalHeap::update_region_ages() {
  ShenandoahMarkingContext *ctx = complete_marking_context();
  for (size_t i = 0; i < num_regions(); i++) {
    ShenandoahHeapRegion *r = get_region(i);
    if (r->is_active() && r->is_young()) {
      HeapWord* tams = ctx->top_at_mark_start(r);
      HeapWord* top = r->top();
      if (top > tams) {
        r->reset_age();
      } else if (is_aging_cycle()) {
        r->increment_age();
      }
    }
  }
}
