// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/v8.h"

#include "src/base/atomicops.h"
#include "src/base/bits.h"
#include "src/code-stubs.h"
#include "src/compilation-cache.h"
#include "src/cpu-profiler.h"
#include "src/deoptimizer.h"
#include "src/execution.h"
#include "src/gdb-jit.h"
#include "src/global-handles.h"
#include "src/heap/incremental-marking.h"
#include "src/heap/mark-compact.h"
#include "src/heap/objects-visiting.h"
#include "src/heap/objects-visiting-inl.h"
#include "src/heap/spaces-inl.h"
#include "src/heap-profiler.h"
#include "src/ic/ic.h"
#include "src/ic/stub-cache.h"

namespace v8 {
namespace internal {


const char* Marking::kWhiteBitPattern = "00";
const char* Marking::kBlackBitPattern = "10";
const char* Marking::kGreyBitPattern = "11";
const char* Marking::kImpossibleBitPattern = "01";


// -------------------------------------------------------------------------
// MarkCompactCollector

MarkCompactCollector::MarkCompactCollector(Heap* heap)
    :  // NOLINT
#ifdef DEBUG
      state_(IDLE),
#endif
      reduce_memory_footprint_(false),
      abort_incremental_marking_(false),
      marking_parity_(ODD_MARKING_PARITY),
      compacting_(false),
      was_marked_incrementally_(false),
      sweeping_in_progress_(false),
      pending_sweeper_jobs_semaphore_(0),
      evacuation_(false),
      migration_slots_buffer_(NULL),
      heap_(heap),
      marking_deque_memory_(NULL),
      marking_deque_memory_committed_(false),
      code_flusher_(NULL),
      have_code_to_deoptimize_(false) {
}

#ifdef VERIFY_HEAP
class VerifyMarkingVisitor : public ObjectVisitor {
 public:
  explicit VerifyMarkingVisitor(Heap* heap) : heap_(heap) {}

  void VisitPointers(Object** start, Object** end) {
    for (Object** current = start; current < end; current++) {
      if ((*current)->IsHeapObject()) {
        HeapObject* object = HeapObject::cast(*current);
        CHECK(heap_->mark_compact_collector()->IsMarked(object));
      }
    }
  }

  void VisitEmbeddedPointer(RelocInfo* rinfo) {
    DCHECK(rinfo->rmode() == RelocInfo::EMBEDDED_OBJECT);
    if (!rinfo->host()->IsWeakObject(rinfo->target_object())) {
      Object* p = rinfo->target_object();
      VisitPointer(&p);
    }
  }

  void VisitCell(RelocInfo* rinfo) {
    Code* code = rinfo->host();
    DCHECK(rinfo->rmode() == RelocInfo::CELL);
    if (!code->IsWeakObject(rinfo->target_cell())) {
      ObjectVisitor::VisitCell(rinfo);
    }
  }

 private:
  Heap* heap_;
};


static void VerifyMarking(Heap* heap, Address bottom, Address top) {
  VerifyMarkingVisitor visitor(heap);
  HeapObject* object;
  Address next_object_must_be_here_or_later = bottom;

  for (Address current = bottom; current < top; current += kPointerSize) {
    object = HeapObject::FromAddress(current);
    if (MarkCompactCollector::IsMarked(object)) {
      CHECK(Marking::IsBlack(Marking::MarkBitFrom(object)));
      CHECK(current >= next_object_must_be_here_or_later);
      object->Iterate(&visitor);
      next_object_must_be_here_or_later = current + object->Size();
    }
  }
}


static void VerifyMarking(NewSpace* space) {
  Address end = space->top();
  NewSpacePageIterator it(space->bottom(), end);
  // The bottom position is at the start of its page. Allows us to use
  // page->area_start() as start of range on all pages.
  CHECK_EQ(space->bottom(),
           NewSpacePage::FromAddress(space->bottom())->area_start());
  while (it.has_next()) {
    NewSpacePage* page = it.next();
    Address limit = it.has_next() ? page->area_end() : end;
    CHECK(limit == end || !page->Contains(end));
    VerifyMarking(space->heap(), page->area_start(), limit);
  }
}


static void VerifyMarking(PagedSpace* space) {
  PageIterator it(space);

  while (it.has_next()) {
    Page* p = it.next();
    VerifyMarking(space->heap(), p->area_start(), p->area_end());
  }
}


static void VerifyMarking(Heap* heap) {
  VerifyMarking(heap->old_pointer_space());
  VerifyMarking(heap->old_data_space());
  VerifyMarking(heap->code_space());
  VerifyMarking(heap->cell_space());
  VerifyMarking(heap->map_space());
  VerifyMarking(heap->new_space());

  VerifyMarkingVisitor visitor(heap);

  LargeObjectIterator it(heap->lo_space());
  for (HeapObject* obj = it.Next(); obj != NULL; obj = it.Next()) {
    if (MarkCompactCollector::IsMarked(obj)) {
      obj->Iterate(&visitor);
    }
  }

  heap->IterateStrongRoots(&visitor, VISIT_ONLY_STRONG);
}


class VerifyEvacuationVisitor : public ObjectVisitor {
 public:
  void VisitPointers(Object** start, Object** end) {
    for (Object** current = start; current < end; current++) {
      if ((*current)->IsHeapObject()) {
        HeapObject* object = HeapObject::cast(*current);
        CHECK(!MarkCompactCollector::IsOnEvacuationCandidate(object));
      }
    }
  }
};


static void VerifyEvacuation(Page* page) {
  VerifyEvacuationVisitor visitor;
  HeapObjectIterator iterator(page, NULL);
  for (HeapObject* heap_object = iterator.Next(); heap_object != NULL;
       heap_object = iterator.Next()) {
    // We skip free space objects.
    if (!heap_object->IsFiller()) {
      heap_object->Iterate(&visitor);
    }
  }
}


static void VerifyEvacuation(NewSpace* space) {
  NewSpacePageIterator it(space->bottom(), space->top());
  VerifyEvacuationVisitor visitor;

  while (it.has_next()) {
    NewSpacePage* page = it.next();
    Address current = page->area_start();
    Address limit = it.has_next() ? page->area_end() : space->top();
    CHECK(limit == space->top() || !page->Contains(space->top()));
    while (current < limit) {
      HeapObject* object = HeapObject::FromAddress(current);
      object->Iterate(&visitor);
      current += object->Size();
    }
  }
}


static void VerifyEvacuation(Heap* heap, PagedSpace* space) {
  if (FLAG_use_allocation_folding &&
      (space == heap->old_pointer_space() || space == heap->old_data_space())) {
    return;
  }
  PageIterator it(space);

  while (it.has_next()) {
    Page* p = it.next();
    if (p->IsEvacuationCandidate()) continue;
    VerifyEvacuation(p);
  }
}


static void VerifyEvacuation(Heap* heap) {
  VerifyEvacuation(heap, heap->old_pointer_space());
  VerifyEvacuation(heap, heap->old_data_space());
  VerifyEvacuation(heap, heap->code_space());
  VerifyEvacuation(heap, heap->cell_space());
  VerifyEvacuation(heap, heap->map_space());
  VerifyEvacuation(heap->new_space());

  VerifyEvacuationVisitor visitor;
  heap->IterateStrongRoots(&visitor, VISIT_ALL);
}
#endif  // VERIFY_HEAP


void MarkCompactCollector::SetUp() {
  free_list_old_data_space_.Reset(new FreeList(heap_->old_data_space()));
  free_list_old_pointer_space_.Reset(new FreeList(heap_->old_pointer_space()));
}


void MarkCompactCollector::TearDown() {
  AbortCompaction();
  delete marking_deque_memory_;
}


void MarkCompactCollector::AddEvacuationCandidate(Page* p) {
  DCHECK(!p->NeverEvacuate());
  p->MarkEvacuationCandidate();
  evacuation_candidates_.Add(p);
}


static void TraceFragmentation(PagedSpace* space) {
  int number_of_pages = space->CountTotalPages();
  intptr_t reserved = (number_of_pages * space->AreaSize());
  intptr_t free = reserved - space->SizeOfObjects();
  PrintF("[%s]: %d pages, %d (%.1f%%) free\n",
         AllocationSpaceName(space->identity()), number_of_pages,
         static_cast<int>(free), static_cast<double>(free) * 100 / reserved);
}


bool MarkCompactCollector::StartCompaction(CompactionMode mode) {
  if (!compacting_) {
    DCHECK(evacuation_candidates_.length() == 0);

    CollectEvacuationCandidates(heap()->old_pointer_space());
    CollectEvacuationCandidates(heap()->old_data_space());

    if (FLAG_compact_code_space && (mode == NON_INCREMENTAL_COMPACTION ||
                                    FLAG_incremental_code_compaction)) {
      CollectEvacuationCandidates(heap()->code_space());
    } else if (FLAG_trace_fragmentation) {
      TraceFragmentation(heap()->code_space());
    }

    if (FLAG_trace_fragmentation) {
      TraceFragmentation(heap()->map_space());
      TraceFragmentation(heap()->cell_space());
    }

    heap()->old_pointer_space()->EvictEvacuationCandidatesFromFreeLists();
    heap()->old_data_space()->EvictEvacuationCandidatesFromFreeLists();
    heap()->code_space()->EvictEvacuationCandidatesFromFreeLists();

    compacting_ = evacuation_candidates_.length() > 0;
  }

  return compacting_;
}


void MarkCompactCollector::CollectGarbage() {
  // Make sure that Prepare() has been called. The individual steps below will
  // update the state as they proceed.
  DCHECK(state_ == PREPARE_GC);

  MarkLiveObjects();
  DCHECK(heap_->incremental_marking()->IsStopped());

  // ClearNonLiveReferences can deoptimize code in dependent code arrays.
  // Process weak cells before so that weak cells in dependent code
  // arrays are cleared or contain only live code objects.
  ProcessAndClearWeakCells();

  if (FLAG_collect_maps) ClearNonLiveReferences();

  ClearWeakCollections();

  heap_->set_encountered_weak_cells(Smi::FromInt(0));

#ifdef VERIFY_HEAP
  if (FLAG_verify_heap) {
    VerifyMarking(heap_);
  }
#endif

  heap_->store_buffer()->ClearInvalidStoreBufferEntries();

#ifdef VERIFY_HEAP
  if (FLAG_verify_heap) {
    heap_->store_buffer()->VerifyValidStoreBufferEntries();
  }
#endif

  SweepSpaces();

#ifdef VERIFY_HEAP
  VerifyWeakEmbeddedObjectsInCode();
  if (FLAG_collect_maps && FLAG_omit_map_checks_for_leaf_maps) {
    VerifyOmittedMapChecks();
  }
#endif

  Finish();

  if (marking_parity_ == EVEN_MARKING_PARITY) {
    marking_parity_ = ODD_MARKING_PARITY;
  } else {
    DCHECK(marking_parity_ == ODD_MARKING_PARITY);
    marking_parity_ = EVEN_MARKING_PARITY;
  }
}


#ifdef VERIFY_HEAP
void MarkCompactCollector::VerifyMarkbitsAreClean(PagedSpace* space) {
  PageIterator it(space);

  while (it.has_next()) {
    Page* p = it.next();
    CHECK(p->markbits()->IsClean());
    CHECK_EQ(0, p->LiveBytes());
  }
}


void MarkCompactCollector::VerifyMarkbitsAreClean(NewSpace* space) {
  NewSpacePageIterator it(space->bottom(), space->top());

  while (it.has_next()) {
    NewSpacePage* p = it.next();
    CHECK(p->markbits()->IsClean());
    CHECK_EQ(0, p->LiveBytes());
  }
}


void MarkCompactCollector::VerifyMarkbitsAreClean() {
  VerifyMarkbitsAreClean(heap_->old_pointer_space());
  VerifyMarkbitsAreClean(heap_->old_data_space());
  VerifyMarkbitsAreClean(heap_->code_space());
  VerifyMarkbitsAreClean(heap_->cell_space());
  VerifyMarkbitsAreClean(heap_->map_space());
  VerifyMarkbitsAreClean(heap_->new_space());

  LargeObjectIterator it(heap_->lo_space());
  for (HeapObject* obj = it.Next(); obj != NULL; obj = it.Next()) {
    MarkBit mark_bit = Marking::MarkBitFrom(obj);
    CHECK(Marking::IsWhite(mark_bit));
    CHECK_EQ(0, Page::FromAddress(obj->address())->LiveBytes());
  }
}


void MarkCompactCollector::VerifyWeakEmbeddedObjectsInCode() {
  HeapObjectIterator code_iterator(heap()->code_space());
  for (HeapObject* obj = code_iterator.Next(); obj != NULL;
       obj = code_iterator.Next()) {
    Code* code = Code::cast(obj);
    if (!code->is_optimized_code()) continue;
    if (WillBeDeoptimized(code)) continue;
    code->VerifyEmbeddedObjectsDependency();
  }
}


void MarkCompactCollector::VerifyOmittedMapChecks() {
  HeapObjectIterator iterator(heap()->map_space());
  for (HeapObject* obj = iterator.Next(); obj != NULL; obj = iterator.Next()) {
    Map* map = Map::cast(obj);
    map->VerifyOmittedMapChecks();
  }
}
#endif  // VERIFY_HEAP


static void ClearMarkbitsInPagedSpace(PagedSpace* space) {
  PageIterator it(space);

  while (it.has_next()) {
    Bitmap::Clear(it.next());
  }
}


static void ClearMarkbitsInNewSpace(NewSpace* space) {
  NewSpacePageIterator it(space->ToSpaceStart(), space->ToSpaceEnd());

  while (it.has_next()) {
    Bitmap::Clear(it.next());
  }
}


void MarkCompactCollector::ClearMarkbits() {
  ClearMarkbitsInPagedSpace(heap_->code_space());
  ClearMarkbitsInPagedSpace(heap_->map_space());
  ClearMarkbitsInPagedSpace(heap_->old_pointer_space());
  ClearMarkbitsInPagedSpace(heap_->old_data_space());
  ClearMarkbitsInPagedSpace(heap_->cell_space());
  ClearMarkbitsInNewSpace(heap_->new_space());

  LargeObjectIterator it(heap_->lo_space());
  for (HeapObject* obj = it.Next(); obj != NULL; obj = it.Next()) {
    MarkBit mark_bit = Marking::MarkBitFrom(obj);
    mark_bit.Clear();
    mark_bit.Next().Clear();
    Page::FromAddress(obj->address())->ResetProgressBar();
    Page::FromAddress(obj->address())->ResetLiveBytes();
  }
}


class MarkCompactCollector::SweeperTask : public v8::Task {
 public:
  SweeperTask(Heap* heap, PagedSpace* space) : heap_(heap), space_(space) {}

  virtual ~SweeperTask() {}

 private:
  // v8::Task overrides.
  void Run() OVERRIDE {
    heap_->mark_compact_collector()->SweepInParallel(space_, 0);
    heap_->mark_compact_collector()->pending_sweeper_jobs_semaphore_.Signal();
  }

  Heap* heap_;
  PagedSpace* space_;

  DISALLOW_COPY_AND_ASSIGN(SweeperTask);
};


void MarkCompactCollector::StartSweeperThreads() {
  DCHECK(free_list_old_pointer_space_.get()->IsEmpty());
  DCHECK(free_list_old_data_space_.get()->IsEmpty());
  V8::GetCurrentPlatform()->CallOnBackgroundThread(
      new SweeperTask(heap(), heap()->old_data_space()),
      v8::Platform::kShortRunningTask);
  V8::GetCurrentPlatform()->CallOnBackgroundThread(
      new SweeperTask(heap(), heap()->old_pointer_space()),
      v8::Platform::kShortRunningTask);
}


void MarkCompactCollector::EnsureSweepingCompleted() {
  DCHECK(sweeping_in_progress_ == true);

  // If sweeping is not completed or not running at all, we try to complete it
  // here.
  if (!heap()->concurrent_sweeping_enabled() || !IsSweepingCompleted()) {
    SweepInParallel(heap()->paged_space(OLD_DATA_SPACE), 0);
    SweepInParallel(heap()->paged_space(OLD_POINTER_SPACE), 0);
  }
  // Wait twice for both jobs.
  if (heap()->concurrent_sweeping_enabled()) {
    pending_sweeper_jobs_semaphore_.Wait();
    pending_sweeper_jobs_semaphore_.Wait();
  }
  ParallelSweepSpacesComplete();
  sweeping_in_progress_ = false;
  RefillFreeList(heap()->paged_space(OLD_DATA_SPACE));
  RefillFreeList(heap()->paged_space(OLD_POINTER_SPACE));
  heap()->paged_space(OLD_DATA_SPACE)->ResetUnsweptFreeBytes();
  heap()->paged_space(OLD_POINTER_SPACE)->ResetUnsweptFreeBytes();

#ifdef VERIFY_HEAP
  if (FLAG_verify_heap && !evacuation()) {
    VerifyEvacuation(heap_);
  }
#endif
}


bool MarkCompactCollector::IsSweepingCompleted() {
  if (!pending_sweeper_jobs_semaphore_.WaitFor(
          base::TimeDelta::FromSeconds(0))) {
    return false;
  }
  pending_sweeper_jobs_semaphore_.Signal();
  return true;
}


void MarkCompactCollector::RefillFreeList(PagedSpace* space) {
  FreeList* free_list;

  if (space == heap()->old_pointer_space()) {
    free_list = free_list_old_pointer_space_.get();
  } else if (space == heap()->old_data_space()) {
    free_list = free_list_old_data_space_.get();
  } else {
    // Any PagedSpace might invoke RefillFreeLists, so we need to make sure
    // to only refill them for old data and pointer spaces.
    return;
  }

  intptr_t freed_bytes = space->free_list()->Concatenate(free_list);
  space->AddToAccountingStats(freed_bytes);
  space->DecrementUnsweptFreeBytes(freed_bytes);
}


void Marking::TransferMark(Address old_start, Address new_start) {
  // This is only used when resizing an object.
  DCHECK(MemoryChunk::FromAddress(old_start) ==
         MemoryChunk::FromAddress(new_start));

  if (!heap_->incremental_marking()->IsMarking()) return;

  // If the mark doesn't move, we don't check the color of the object.
  // It doesn't matter whether the object is black, since it hasn't changed
  // size, so the adjustment to the live data count will be zero anyway.
  if (old_start == new_start) return;

  MarkBit new_mark_bit = MarkBitFrom(new_start);
  MarkBit old_mark_bit = MarkBitFrom(old_start);

#ifdef DEBUG
  ObjectColor old_color = Color(old_mark_bit);
#endif

  if (Marking::IsBlack(old_mark_bit)) {
    old_mark_bit.Clear();
    DCHECK(IsWhite(old_mark_bit));
    Marking::MarkBlack(new_mark_bit);
    return;
  } else if (Marking::IsGrey(old_mark_bit)) {
    old_mark_bit.Clear();
    old_mark_bit.Next().Clear();
    DCHECK(IsWhite(old_mark_bit));
    heap_->incremental_marking()->WhiteToGreyAndPush(
        HeapObject::FromAddress(new_start), new_mark_bit);
    heap_->incremental_marking()->RestartIfNotMarking();
  }

#ifdef DEBUG
  ObjectColor new_color = Color(new_mark_bit);
  DCHECK(new_color == old_color);
#endif
}


const char* AllocationSpaceName(AllocationSpace space) {
  switch (space) {
    case NEW_SPACE:
      return "NEW_SPACE";
    case OLD_POINTER_SPACE:
      return "OLD_POINTER_SPACE";
    case OLD_DATA_SPACE:
      return "OLD_DATA_SPACE";
    case CODE_SPACE:
      return "CODE_SPACE";
    case MAP_SPACE:
      return "MAP_SPACE";
    case CELL_SPACE:
      return "CELL_SPACE";
    case LO_SPACE:
      return "LO_SPACE";
    default:
      UNREACHABLE();
  }

  return NULL;
}


// Returns zero for pages that have so little fragmentation that it is not
// worth defragmenting them.  Otherwise a positive integer that gives an
// estimate of fragmentation on an arbitrary scale.
static int FreeListFragmentation(PagedSpace* space, Page* p) {
  // If page was not swept then there are no free list items on it.
  if (!p->WasSwept()) {
    if (FLAG_trace_fragmentation_verbose) {
      PrintF("%p [%s]: %d bytes live (unswept)\n", reinterpret_cast<void*>(p),
             AllocationSpaceName(space->identity()), p->LiveBytes());
    }
    return FLAG_always_compact ? 1 : 0;
  }

  PagedSpace::SizeStats sizes;
  space->ObtainFreeListStatistics(p, &sizes);

  intptr_t ratio;
  intptr_t ratio_threshold;
  intptr_t area_size = space->AreaSize();
  if (space->identity() == CODE_SPACE) {
    ratio = (sizes.medium_size_ * 10 + sizes.large_size_ * 2) * 100 / area_size;
    ratio_threshold = 10;
  } else {
    ratio = (sizes.small_size_ * 5 + sizes.medium_size_) * 100 / area_size;
    ratio_threshold = 15;
  }

  if (FLAG_trace_fragmentation_verbose) {
    PrintF("%p [%s]: %d (%.2f%%) %d (%.2f%%) %d (%.2f%%) %d (%.2f%%) %s\n",
           reinterpret_cast<void*>(p), AllocationSpaceName(space->identity()),
           static_cast<int>(sizes.small_size_),
           static_cast<double>(sizes.small_size_ * 100) / area_size,
           static_cast<int>(sizes.medium_size_),
           static_cast<double>(sizes.medium_size_ * 100) / area_size,
           static_cast<int>(sizes.large_size_),
           static_cast<double>(sizes.large_size_ * 100) / area_size,
           static_cast<int>(sizes.huge_size_),
           static_cast<double>(sizes.huge_size_ * 100) / area_size,
           (ratio > ratio_threshold) ? "[fragmented]" : "");
  }

  if (FLAG_always_compact && sizes.Total() != area_size) {
    return 1;
  }

  if (ratio <= ratio_threshold) return 0;  // Not fragmented.

  return static_cast<int>(ratio - ratio_threshold);
}


void MarkCompactCollector::CollectEvacuationCandidates(PagedSpace* space) {
  DCHECK(space->identity() == OLD_POINTER_SPACE ||
         space->identity() == OLD_DATA_SPACE ||
         space->identity() == CODE_SPACE);

  static const int kMaxMaxEvacuationCandidates = 1000;
  int number_of_pages = space->CountTotalPages();
  int max_evacuation_candidates =
      static_cast<int>(std::sqrt(number_of_pages / 2.0) + 1);

  if (FLAG_stress_compaction || FLAG_always_compact) {
    max_evacuation_candidates = kMaxMaxEvacuationCandidates;
  }

  class Candidate {
   public:
    Candidate() : fragmentation_(0), page_(NULL) {}
    Candidate(int f, Page* p) : fragmentation_(f), page_(p) {}

    int fragmentation() { return fragmentation_; }
    Page* page() { return page_; }

   private:
    int fragmentation_;
    Page* page_;
  };

  enum CompactionMode { COMPACT_FREE_LISTS, REDUCE_MEMORY_FOOTPRINT };

  CompactionMode mode = COMPACT_FREE_LISTS;

  intptr_t reserved = number_of_pages * space->AreaSize();
  intptr_t over_reserved = reserved - space->SizeOfObjects();
  static const intptr_t kFreenessThreshold = 50;

  if (reduce_memory_footprint_ && over_reserved >= space->AreaSize()) {
    // If reduction of memory footprint was requested, we are aggressive
    // about choosing pages to free.  We expect that half-empty pages
    // are easier to compact so slightly bump the limit.
    mode = REDUCE_MEMORY_FOOTPRINT;
    max_evacuation_candidates += 2;
  }


  if (over_reserved > reserved / 3 && over_reserved >= 2 * space->AreaSize()) {
    // If over-usage is very high (more than a third of the space), we
    // try to free all mostly empty pages.  We expect that almost empty
    // pages are even easier to compact so bump the limit even more.
    mode = REDUCE_MEMORY_FOOTPRINT;
    max_evacuation_candidates *= 2;
  }

  if (FLAG_always_compact) {
    max_evacuation_candidates = kMaxMaxEvacuationCandidates;
  }

  if (FLAG_trace_fragmentation && mode == REDUCE_MEMORY_FOOTPRINT) {
    PrintF(
        "Estimated over reserved memory: %.1f / %.1f MB (threshold %d), "
        "evacuation candidate limit: %d\n",
        static_cast<double>(over_reserved) / MB,
        static_cast<double>(reserved) / MB,
        static_cast<int>(kFreenessThreshold), max_evacuation_candidates);
  }

  intptr_t estimated_release = 0;

  Candidate candidates[kMaxMaxEvacuationCandidates];

  if (FLAG_trace_fragmentation &&
      max_evacuation_candidates >= kMaxMaxEvacuationCandidates) {
    PrintF("Hit max page compaction limit of %d pages\n",
           kMaxMaxEvacuationCandidates);
  }
  max_evacuation_candidates =
      Min(kMaxMaxEvacuationCandidates, max_evacuation_candidates);

  int count = 0;
  int fragmentation = 0;
  int page_number = 0;
  Candidate* least = NULL;

  PageIterator it(space);
  while (it.has_next()) {
    Page* p = it.next();
    if (p->NeverEvacuate()) continue;

    // Invariant: Evacuation candidates are just created when marking is
    // started. At the end of a GC all evacuation candidates are cleared and
    // their slot buffers are released.
    CHECK(!p->IsEvacuationCandidate());
    CHECK(p->slots_buffer() == NULL);

    if (FLAG_stress_compaction) {
      if (FLAG_manual_evacuation_candidates_selection) {
        if (p->IsFlagSet(MemoryChunk::FORCE_EVACUATION_CANDIDATE_FOR_TESTING)) {
          p->ClearFlag(MemoryChunk::FORCE_EVACUATION_CANDIDATE_FOR_TESTING);
          fragmentation = 1;
        }
      } else {
        unsigned int counter = space->heap()->ms_count();
        if ((counter & 1) == (page_number & 1)) fragmentation = 1;
        page_number++;
      }
    } else if (mode == REDUCE_MEMORY_FOOTPRINT && !FLAG_always_compact) {
      // Don't try to release too many pages.
      if (estimated_release >= over_reserved) {
        continue;
      }

      intptr_t free_bytes = 0;

      if (!p->WasSwept()) {
        free_bytes = (p->area_size() - p->LiveBytes());
      } else {
        PagedSpace::SizeStats sizes;
        space->ObtainFreeListStatistics(p, &sizes);
        free_bytes = sizes.Total();
      }

      int free_pct = static_cast<int>(free_bytes * 100) / p->area_size();

      if (free_pct >= kFreenessThreshold) {
        estimated_release += free_bytes;
        fragmentation = free_pct;
      } else {
        fragmentation = 0;
      }

      if (FLAG_trace_fragmentation_verbose) {
        PrintF("%p [%s]: %d (%.2f%%) free %s\n", reinterpret_cast<void*>(p),
               AllocationSpaceName(space->identity()),
               static_cast<int>(free_bytes),
               static_cast<double>(free_bytes * 100) / p->area_size(),
               (fragmentation > 0) ? "[fragmented]" : "");
      }
    } else {
      fragmentation = FreeListFragmentation(space, p);
    }

    if (fragmentation != 0) {
      if (count < max_evacuation_candidates) {
        candidates[count++] = Candidate(fragmentation, p);
      } else {
        if (least == NULL) {
          for (int i = 0; i < max_evacuation_candidates; i++) {
            if (least == NULL ||
                candidates[i].fragmentation() < least->fragmentation()) {
              least = candidates + i;
            }
          }
        }
        if (least->fragmentation() < fragmentation) {
          *least = Candidate(fragmentation, p);
          least = NULL;
        }
      }
    }
  }

  for (int i = 0; i < count; i++) {
    AddEvacuationCandidate(candidates[i].page());
  }

  if (count > 0 && FLAG_trace_fragmentation) {
    PrintF("Collected %d evacuation candidates for space %s\n", count,
           AllocationSpaceName(space->identity()));
  }
}


void MarkCompactCollector::AbortCompaction() {
  if (compacting_) {
    int npages = evacuation_candidates_.length();
    for (int i = 0; i < npages; i++) {
      Page* p = evacuation_candidates_[i];
      slots_buffer_allocator_.DeallocateChain(p->slots_buffer_address());
      p->ClearEvacuationCandidate();
      p->ClearFlag(MemoryChunk::RESCAN_ON_EVACUATION);
    }
    compacting_ = false;
    evacuation_candidates_.Rewind(0);
    invalidated_code_.Rewind(0);
  }
  DCHECK_EQ(0, evacuation_candidates_.length());
}


void MarkCompactCollector::Prepare() {
  was_marked_incrementally_ = heap()->incremental_marking()->IsMarking();

#ifdef DEBUG
  DCHECK(state_ == IDLE);
  state_ = PREPARE_GC;
#endif

  DCHECK(!FLAG_never_compact || !FLAG_always_compact);

  if (sweeping_in_progress()) {
    // Instead of waiting we could also abort the sweeper threads here.
    EnsureSweepingCompleted();
  }

  // Clear marking bits if incremental marking is aborted.
  if (was_marked_incrementally_ && abort_incremental_marking_) {
    heap()->incremental_marking()->Abort();
    ClearMarkbits();
    AbortWeakCollections();
    AbortWeakCells();
    AbortCompaction();
    was_marked_incrementally_ = false;
  }

  // Don't start compaction if we are in the middle of incremental
  // marking cycle. We did not collect any slots.
  if (!FLAG_never_compact && !was_marked_incrementally_) {
    StartCompaction(NON_INCREMENTAL_COMPACTION);
  }

  PagedSpaces spaces(heap());
  for (PagedSpace* space = spaces.next(); space != NULL;
       space = spaces.next()) {
    space->PrepareForMarkCompact();
  }

#ifdef VERIFY_HEAP
  if (!was_marked_incrementally_ && FLAG_verify_heap) {
    VerifyMarkbitsAreClean();
  }
#endif
}


void MarkCompactCollector::Finish() {
#ifdef DEBUG
  DCHECK(state_ == SWEEP_SPACES || state_ == RELOCATE_OBJECTS);
  state_ = IDLE;
#endif
  // The stub cache is not traversed during GC; clear the cache to
  // force lazy re-initialization of it. This must be done after the
  // GC, because it relies on the new address of certain old space
  // objects (empty string, illegal builtin).
  isolate()->stub_cache()->Clear();

  if (have_code_to_deoptimize_) {
    // Some code objects were marked for deoptimization during the GC.
    Deoptimizer::DeoptimizeMarkedCode(isolate());
    have_code_to_deoptimize_ = false;
  }

  heap_->incremental_marking()->ClearIdleMarkingDelayCounter();
}


// -------------------------------------------------------------------------
// Phase 1: tracing and marking live objects.
//   before: all objects are in normal state.
//   after: a live object's map pointer is marked as '00'.

// Marking all live objects in the heap as part of mark-sweep or mark-compact
// collection.  Before marking, all objects are in their normal state.  After
// marking, live objects' map pointers are marked indicating that the object
// has been found reachable.
//
// The marking algorithm is a (mostly) depth-first (because of possible stack
// overflow) traversal of the graph of objects reachable from the roots.  It
// uses an explicit stack of pointers rather than recursion.  The young
// generation's inactive ('from') space is used as a marking stack.  The
// objects in the marking stack are the ones that have been reached and marked
// but their children have not yet been visited.
//
// The marking stack can overflow during traversal.  In that case, we set an
// overflow flag.  When the overflow flag is set, we continue marking objects
// reachable from the objects on the marking stack, but no longer push them on
// the marking stack.  Instead, we mark them as both marked and overflowed.
// When the stack is in the overflowed state, objects marked as overflowed
// have been reached and marked but their children have not been visited yet.
// After emptying the marking stack, we clear the overflow flag and traverse
// the heap looking for objects marked as overflowed, push them on the stack,
// and continue with marking.  This process repeats until all reachable
// objects have been marked.

void CodeFlusher::ProcessJSFunctionCandidates() {
  Code* lazy_compile = isolate_->builtins()->builtin(Builtins::kCompileLazy);
  Object* undefined = isolate_->heap()->undefined_value();

  JSFunction* candidate = jsfunction_candidates_head_;
  JSFunction* next_candidate;
  while (candidate != NULL) {
    next_candidate = GetNextCandidate(candidate);
    ClearNextCandidate(candidate, undefined);

    SharedFunctionInfo* shared = candidate->shared();

    Code* code = shared->code();
    MarkBit code_mark = Marking::MarkBitFrom(code);
    if (!code_mark.Get()) {
      if (FLAG_trace_code_flushing && shared->is_compiled()) {
        PrintF("[code-flushing clears: ");
        shared->ShortPrint();
        PrintF(" - age: %d]\n", code->GetAge());
      }
      shared->set_code(lazy_compile);
      candidate->set_code(lazy_compile);
    } else {
      candidate->set_code(code);
    }

    // We are in the middle of a GC cycle so the write barrier in the code
    // setter did not record the slot update and we have to do that manually.
    Address slot = candidate->address() + JSFunction::kCodeEntryOffset;
    Code* target = Code::cast(Code::GetObjectFromEntryAddress(slot));
    isolate_->heap()->mark_compact_collector()->RecordCodeEntrySlot(slot,
                                                                    target);

    Object** shared_code_slot =
        HeapObject::RawField(shared, SharedFunctionInfo::kCodeOffset);
    isolate_->heap()->mark_compact_collector()->RecordSlot(
        shared_code_slot, shared_code_slot, *shared_code_slot);

    candidate = next_candidate;
  }

  jsfunction_candidates_head_ = NULL;
}


void CodeFlusher::ProcessSharedFunctionInfoCandidates() {
  Code* lazy_compile = isolate_->builtins()->builtin(Builtins::kCompileLazy);

  SharedFunctionInfo* candidate = shared_function_info_candidates_head_;
  SharedFunctionInfo* next_candidate;
  while (candidate != NULL) {
    next_candidate = GetNextCandidate(candidate);
    ClearNextCandidate(candidate);

    Code* code = candidate->code();
    MarkBit code_mark = Marking::MarkBitFrom(code);
    if (!code_mark.Get()) {
      if (FLAG_trace_code_flushing && candidate->is_compiled()) {
        PrintF("[code-flushing clears: ");
        candidate->ShortPrint();
        PrintF(" - age: %d]\n", code->GetAge());
      }
      candidate->set_code(lazy_compile);
    }

    Object** code_slot =
        HeapObject::RawField(candidate, SharedFunctionInfo::kCodeOffset);
    isolate_->heap()->mark_compact_collector()->RecordSlot(code_slot, code_slot,
                                                           *code_slot);

    candidate = next_candidate;
  }

  shared_function_info_candidates_head_ = NULL;
}


void CodeFlusher::ProcessOptimizedCodeMaps() {
  STATIC_ASSERT(SharedFunctionInfo::kEntryLength == 4);

  SharedFunctionInfo* holder = optimized_code_map_holder_head_;
  SharedFunctionInfo* next_holder;

  while (holder != NULL) {
    next_holder = GetNextCodeMap(holder);
    ClearNextCodeMap(holder);

    FixedArray* code_map = FixedArray::cast(holder->optimized_code_map());
    int new_length = SharedFunctionInfo::kEntriesStart;
    int old_length = code_map->length();
    for (int i = SharedFunctionInfo::kEntriesStart; i < old_length;
         i += SharedFunctionInfo::kEntryLength) {
      Code* code =
          Code::cast(code_map->get(i + SharedFunctionInfo::kCachedCodeOffset));
      if (!Marking::MarkBitFrom(code).Get()) continue;

      // Move every slot in the entry.
      for (int j = 0; j < SharedFunctionInfo::kEntryLength; j++) {
        int dst_index = new_length++;
        Object** slot = code_map->RawFieldOfElementAt(dst_index);
        Object* object = code_map->get(i + j);
        code_map->set(dst_index, object);
        if (j == SharedFunctionInfo::kOsrAstIdOffset) {
          DCHECK(object->IsSmi());
        } else {
          DCHECK(
              Marking::IsBlack(Marking::MarkBitFrom(HeapObject::cast(*slot))));
          isolate_->heap()->mark_compact_collector()->RecordSlot(slot, slot,
                                                                 *slot);
        }
      }
    }

    // Trim the optimized code map if entries have been removed.
    if (new_length < old_length) {
      holder->TrimOptimizedCodeMap(old_length - new_length);
    }

    holder = next_holder;
  }

  optimized_code_map_holder_head_ = NULL;
}


void CodeFlusher::EvictCandidate(SharedFunctionInfo* shared_info) {
  // Make sure previous flushing decisions are revisited.
  isolate_->heap()->incremental_marking()->RecordWrites(shared_info);

  if (FLAG_trace_code_flushing) {
    PrintF("[code-flushing abandons function-info: ");
    shared_info->ShortPrint();
    PrintF("]\n");
  }

  SharedFunctionInfo* candidate = shared_function_info_candidates_head_;
  SharedFunctionInfo* next_candidate;
  if (candidate == shared_info) {
    next_candidate = GetNextCandidate(shared_info);
    shared_function_info_candidates_head_ = next_candidate;
    ClearNextCandidate(shared_info);
  } else {
    while (candidate != NULL) {
      next_candidate = GetNextCandidate(candidate);

      if (next_candidate == shared_info) {
        next_candidate = GetNextCandidate(shared_info);
        SetNextCandidate(candidate, next_candidate);
        ClearNextCandidate(shared_info);
        break;
      }

      candidate = next_candidate;
    }
  }
}


void CodeFlusher::EvictCandidate(JSFunction* function) {
  DCHECK(!function->next_function_link()->IsUndefined());
  Object* undefined = isolate_->heap()->undefined_value();

  // Make sure previous flushing decisions are revisited.
  isolate_->heap()->incremental_marking()->RecordWrites(function);
  isolate_->heap()->incremental_marking()->RecordWrites(function->shared());

  if (FLAG_trace_code_flushing) {
    PrintF("[code-flushing abandons closure: ");
    function->shared()->ShortPrint();
    PrintF("]\n");
  }

  JSFunction* candidate = jsfunction_candidates_head_;
  JSFunction* next_candidate;
  if (candidate == function) {
    next_candidate = GetNextCandidate(function);
    jsfunction_candidates_head_ = next_candidate;
    ClearNextCandidate(function, undefined);
  } else {
    while (candidate != NULL) {
      next_candidate = GetNextCandidate(candidate);

      if (next_candidate == function) {
        next_candidate = GetNextCandidate(function);
        SetNextCandidate(candidate, next_candidate);
        ClearNextCandidate(function, undefined);
        break;
      }

      candidate = next_candidate;
    }
  }
}


void CodeFlusher::EvictOptimizedCodeMap(SharedFunctionInfo* code_map_holder) {
  DCHECK(!FixedArray::cast(code_map_holder->optimized_code_map())
              ->get(SharedFunctionInfo::kNextMapIndex)
              ->IsUndefined());

  // Make sure previous flushing decisions are revisited.
  isolate_->heap()->incremental_marking()->RecordWrites(code_map_holder);

  if (FLAG_trace_code_flushing) {
    PrintF("[code-flushing abandons code-map: ");
    code_map_holder->ShortPrint();
    PrintF("]\n");
  }

  SharedFunctionInfo* holder = optimized_code_map_holder_head_;
  SharedFunctionInfo* next_holder;
  if (holder == code_map_holder) {
    next_holder = GetNextCodeMap(code_map_holder);
    optimized_code_map_holder_head_ = next_holder;
    ClearNextCodeMap(code_map_holder);
  } else {
    while (holder != NULL) {
      next_holder = GetNextCodeMap(holder);

      if (next_holder == code_map_holder) {
        next_holder = GetNextCodeMap(code_map_holder);
        SetNextCodeMap(holder, next_holder);
        ClearNextCodeMap(code_map_holder);
        break;
      }

      holder = next_holder;
    }
  }
}


void CodeFlusher::EvictJSFunctionCandidates() {
  JSFunction* candidate = jsfunction_candidates_head_;
  JSFunction* next_candidate;
  while (candidate != NULL) {
    next_candidate = GetNextCandidate(candidate);
    EvictCandidate(candidate);
    candidate = next_candidate;
  }
  DCHECK(jsfunction_candidates_head_ == NULL);
}


void CodeFlusher::EvictSharedFunctionInfoCandidates() {
  SharedFunctionInfo* candidate = shared_function_info_candidates_head_;
  SharedFunctionInfo* next_candidate;
  while (candidate != NULL) {
    next_candidate = GetNextCandidate(candidate);
    EvictCandidate(candidate);
    candidate = next_candidate;
  }
  DCHECK(shared_function_info_candidates_head_ == NULL);
}


void CodeFlusher::EvictOptimizedCodeMaps() {
  SharedFunctionInfo* holder = optimized_code_map_holder_head_;
  SharedFunctionInfo* next_holder;
  while (holder != NULL) {
    next_holder = GetNextCodeMap(holder);
    EvictOptimizedCodeMap(holder);
    holder = next_holder;
  }
  DCHECK(optimized_code_map_holder_head_ == NULL);
}


void CodeFlusher::IteratePointersToFromSpace(ObjectVisitor* v) {
  Heap* heap = isolate_->heap();

  JSFunction** slot = &jsfunction_candidates_head_;
  JSFunction* candidate = jsfunction_candidates_head_;
  while (candidate != NULL) {
    if (heap->InFromSpace(candidate)) {
      v->VisitPointer(reinterpret_cast<Object**>(slot));
    }
    candidate = GetNextCandidate(*slot);
    slot = GetNextCandidateSlot(*slot);
  }
}


MarkCompactCollector::~MarkCompactCollector() {
  if (code_flusher_ != NULL) {
    delete code_flusher_;
    code_flusher_ = NULL;
  }
}


static inline HeapObject* ShortCircuitConsString(Object** p) {
  // Optimization: If the heap object pointed to by p is a non-internalized
  // cons string whose right substring is HEAP->empty_string, update
  // it in place to its left substring.  Return the updated value.
  //
  // Here we assume that if we change *p, we replace it with a heap object
  // (i.e., the left substring of a cons string is always a heap object).
  //
  // The check performed is:
  //   object->IsConsString() && !object->IsInternalizedString() &&
  //   (ConsString::cast(object)->second() == HEAP->empty_string())
  // except the maps for the object and its possible substrings might be
  // marked.
  HeapObject* object = HeapObject::cast(*p);
  Map* map = object->map();
  InstanceType type = map->instance_type();
  if (!IsShortcutCandidate(type)) return object;

  Object* second = reinterpret_cast<ConsString*>(object)->second();
  Heap* heap = map->GetHeap();
  if (second != heap->empty_string()) {
    return object;
  }

  // Since we don't have the object's start, it is impossible to update the
  // page dirty marks. Therefore, we only replace the string with its left
  // substring when page dirty marks do not change.
  Object* first = reinterpret_cast<ConsString*>(object)->first();
  if (!heap->InNewSpace(object) && heap->InNewSpace(first)) return object;

  *p = first;
  return HeapObject::cast(first);
}


class MarkCompactMarkingVisitor
    : public StaticMarkingVisitor<MarkCompactMarkingVisitor> {
 public:
  static void ObjectStatsVisitBase(StaticVisitorBase::VisitorId id, Map* map,
                                   HeapObject* obj);

  static void ObjectStatsCountFixedArray(
      FixedArrayBase* fixed_array, FixedArraySubInstanceType fast_type,
      FixedArraySubInstanceType dictionary_type);

  template <MarkCompactMarkingVisitor::VisitorId id>
  class ObjectStatsTracker {
   public:
    static inline void Visit(Map* map, HeapObject* obj);
  };

  static void Initialize();

  INLINE(static void VisitPointer(Heap* heap, Object** p)) {
    MarkObjectByPointer(heap->mark_compact_collector(), p, p);
  }

  INLINE(static void VisitPointers(Heap* heap, Object** start, Object** end)) {
    // Mark all objects pointed to in [start, end).
    const int kMinRangeForMarkingRecursion = 64;
    if (end - start >= kMinRangeForMarkingRecursion) {
      if (VisitUnmarkedObjects(heap, start, end)) return;
      // We are close to a stack overflow, so just mark the objects.
    }
    MarkCompactCollector* collector = heap->mark_compact_collector();
    for (Object** p = start; p < end; p++) {
      MarkObjectByPointer(collector, start, p);
    }
  }

  // Marks the object black and pushes it on the marking stack.
  INLINE(static void MarkObject(Heap* heap, HeapObject* object)) {
    MarkBit mark = Marking::MarkBitFrom(object);
    heap->mark_compact_collector()->MarkObject(object, mark);
  }

  // Marks the object black without pushing it on the marking stack.
  // Returns true if object needed marking and false otherwise.
  INLINE(static bool MarkObjectWithoutPush(Heap* heap, HeapObject* object)) {
    MarkBit mark_bit = Marking::MarkBitFrom(object);
    if (!mark_bit.Get()) {
      heap->mark_compact_collector()->SetMark(object, mark_bit);
      return true;
    }
    return false;
  }

  // Mark object pointed to by p.
  INLINE(static void MarkObjectByPointer(MarkCompactCollector* collector,
                                         Object** anchor_slot, Object** p)) {
    if (!(*p)->IsHeapObject()) return;
    HeapObject* object = ShortCircuitConsString(p);
    collector->RecordSlot(anchor_slot, p, object);
    MarkBit mark = Marking::MarkBitFrom(object);
    collector->MarkObject(object, mark);
  }


  // Visit an unmarked object.
  INLINE(static void VisitUnmarkedObject(MarkCompactCollector* collector,
                                         HeapObject* obj)) {
#ifdef DEBUG
    DCHECK(collector->heap()->Contains(obj));
    DCHECK(!collector->heap()->mark_compact_collector()->IsMarked(obj));
#endif
    Map* map = obj->map();
    Heap* heap = obj->GetHeap();
    MarkBit mark = Marking::MarkBitFrom(obj);
    heap->mark_compact_collector()->SetMark(obj, mark);
    // Mark the map pointer and the body.
    MarkBit map_mark = Marking::MarkBitFrom(map);
    heap->mark_compact_collector()->MarkObject(map, map_mark);
    IterateBody(map, obj);
  }

  // Visit all unmarked objects pointed to by [start, end).
  // Returns false if the operation fails (lack of stack space).
  INLINE(static bool VisitUnmarkedObjects(Heap* heap, Object** start,
                                          Object** end)) {
    // Return false is we are close to the stack limit.
    StackLimitCheck check(heap->isolate());
    if (check.HasOverflowed()) return false;

    MarkCompactCollector* collector = heap->mark_compact_collector();
    // Visit the unmarked objects.
    for (Object** p = start; p < end; p++) {
      Object* o = *p;
      if (!o->IsHeapObject()) continue;
      collector->RecordSlot(start, p, o);
      HeapObject* obj = HeapObject::cast(o);
      MarkBit mark = Marking::MarkBitFrom(obj);
      if (mark.Get()) continue;
      VisitUnmarkedObject(collector, obj);
    }
    return true;
  }

 private:
  template <int id>
  static inline void TrackObjectStatsAndVisit(Map* map, HeapObject* obj);

  // Code flushing support.

  static const int kRegExpCodeThreshold = 5;

  static void UpdateRegExpCodeAgeAndFlush(Heap* heap, JSRegExp* re,
                                          bool is_one_byte) {
    // Make sure that the fixed array is in fact initialized on the RegExp.
    // We could potentially trigger a GC when initializing the RegExp.
    if (HeapObject::cast(re->data())->map()->instance_type() !=
        FIXED_ARRAY_TYPE)
      return;

    // Make sure this is a RegExp that actually contains code.
    if (re->TypeTag() != JSRegExp::IRREGEXP) return;

    Object* code = re->DataAt(JSRegExp::code_index(is_one_byte));
    if (!code->IsSmi() &&
        HeapObject::cast(code)->map()->instance_type() == CODE_TYPE) {
      // Save a copy that can be reinstated if we need the code again.
      re->SetDataAt(JSRegExp::saved_code_index(is_one_byte), code);

      // Saving a copy might create a pointer into compaction candidate
      // that was not observed by marker.  This might happen if JSRegExp data
      // was marked through the compilation cache before marker reached JSRegExp
      // object.
      FixedArray* data = FixedArray::cast(re->data());
      Object** slot =
          data->data_start() + JSRegExp::saved_code_index(is_one_byte);
      heap->mark_compact_collector()->RecordSlot(slot, slot, code);

      // Set a number in the 0-255 range to guarantee no smi overflow.
      re->SetDataAt(JSRegExp::code_index(is_one_byte),
                    Smi::FromInt(heap->sweep_generation() & 0xff));
    } else if (code->IsSmi()) {
      int value = Smi::cast(code)->value();
      // The regexp has not been compiled yet or there was a compilation error.
      if (value == JSRegExp::kUninitializedValue ||
          value == JSRegExp::kCompilationErrorValue) {
        return;
      }

      // Check if we should flush now.
      if (value == ((heap->sweep_generation() - kRegExpCodeThreshold) & 0xff)) {
        re->SetDataAt(JSRegExp::code_index(is_one_byte),
                      Smi::FromInt(JSRegExp::kUninitializedValue));
        re->SetDataAt(JSRegExp::saved_code_index(is_one_byte),
                      Smi::FromInt(JSRegExp::kUninitializedValue));
      }
    }
  }


  // Works by setting the current sweep_generation (as a smi) in the
  // code object place in the data array of the RegExp and keeps a copy
  // around that can be reinstated if we reuse the RegExp before flushing.
  // If we did not use the code for kRegExpCodeThreshold mark sweep GCs
  // we flush the code.
  static void VisitRegExpAndFlushCode(Map* map, HeapObject* object) {
    Heap* heap = map->GetHeap();
    MarkCompactCollector* collector = heap->mark_compact_collector();
    if (!collector->is_code_flushing_enabled()) {
      VisitJSRegExp(map, object);
      return;
    }
    JSRegExp* re = reinterpret_cast<JSRegExp*>(object);
    // Flush code or set age on both one byte and two byte code.
    UpdateRegExpCodeAgeAndFlush(heap, re, true);
    UpdateRegExpCodeAgeAndFlush(heap, re, false);
    // Visit the fields of the RegExp, including the updated FixedArray.
    VisitJSRegExp(map, object);
  }

  static VisitorDispatchTable<Callback> non_count_table_;
};


void MarkCompactMarkingVisitor::ObjectStatsCountFixedArray(
    FixedArrayBase* fixed_array, FixedArraySubInstanceType fast_type,
    FixedArraySubInstanceType dictionary_type) {
  Heap* heap = fixed_array->map()->GetHeap();
  if (fixed_array->map() != heap->fixed_cow_array_map() &&
      fixed_array->map() != heap->fixed_double_array_map() &&
      fixed_array != heap->empty_fixed_array()) {
    if (fixed_array->IsDictionary()) {
      heap->RecordFixedArraySubTypeStats(dictionary_type, fixed_array->Size());
    } else {
      heap->RecordFixedArraySubTypeStats(fast_type, fixed_array->Size());
    }
  }
}


void MarkCompactMarkingVisitor::ObjectStatsVisitBase(
    MarkCompactMarkingVisitor::VisitorId id, Map* map, HeapObject* obj) {
  Heap* heap = map->GetHeap();
  int object_size = obj->Size();
  heap->RecordObjectStats(map->instance_type(), object_size);
  non_count_table_.GetVisitorById(id)(map, obj);
  if (obj->IsJSObject()) {
    JSObject* object = JSObject::cast(obj);
    ObjectStatsCountFixedArray(object->elements(), DICTIONARY_ELEMENTS_SUB_TYPE,
                               FAST_ELEMENTS_SUB_TYPE);
    ObjectStatsCountFixedArray(object->properties(),
                               DICTIONARY_PROPERTIES_SUB_TYPE,
                               FAST_PROPERTIES_SUB_TYPE);
  }
}


template <MarkCompactMarkingVisitor::VisitorId id>
void MarkCompactMarkingVisitor::ObjectStatsTracker<id>::Visit(Map* map,
                                                              HeapObject* obj) {
  ObjectStatsVisitBase(id, map, obj);
}


template <>
class MarkCompactMarkingVisitor::ObjectStatsTracker<
    MarkCompactMarkingVisitor::kVisitMap> {
 public:
  static inline void Visit(Map* map, HeapObject* obj) {
    Heap* heap = map->GetHeap();
    Map* map_obj = Map::cast(obj);
    DCHECK(map->instance_type() == MAP_TYPE);
    DescriptorArray* array = map_obj->instance_descriptors();
    if (map_obj->owns_descriptors() &&
        array != heap->empty_descriptor_array()) {
      int fixed_array_size = array->Size();
      heap->RecordFixedArraySubTypeStats(DESCRIPTOR_ARRAY_SUB_TYPE,
                                         fixed_array_size);
    }
    if (TransitionArray::IsFullTransitionArray(map_obj->raw_transitions())) {
      int fixed_array_size =
          TransitionArray::cast(map_obj->raw_transitions())->Size();
      heap->RecordFixedArraySubTypeStats(TRANSITION_ARRAY_SUB_TYPE,
                                         fixed_array_size);
    }
    if (map_obj->has_code_cache()) {
      CodeCache* cache = CodeCache::cast(map_obj->code_cache());
      heap->RecordFixedArraySubTypeStats(MAP_CODE_CACHE_SUB_TYPE,
                                         cache->default_cache()->Size());
      if (!cache->normal_type_cache()->IsUndefined()) {
        heap->RecordFixedArraySubTypeStats(
            MAP_CODE_CACHE_SUB_TYPE,
            FixedArray::cast(cache->normal_type_cache())->Size());
      }
    }
    ObjectStatsVisitBase(kVisitMap, map, obj);
  }
};


template <>
class MarkCompactMarkingVisitor::ObjectStatsTracker<
    MarkCompactMarkingVisitor::kVisitCode> {
 public:
  static inline void Visit(Map* map, HeapObject* obj) {
    Heap* heap = map->GetHeap();
    int object_size = obj->Size();
    DCHECK(map->instance_type() == CODE_TYPE);
    Code* code_obj = Code::cast(obj);
    heap->RecordCodeSubTypeStats(code_obj->kind(), code_obj->GetRawAge(),
                                 object_size);
    ObjectStatsVisitBase(kVisitCode, map, obj);
  }
};


template <>
class MarkCompactMarkingVisitor::ObjectStatsTracker<
    MarkCompactMarkingVisitor::kVisitSharedFunctionInfo> {
 public:
  static inline void Visit(Map* map, HeapObject* obj) {
    Heap* heap = map->GetHeap();
    SharedFunctionInfo* sfi = SharedFunctionInfo::cast(obj);
    if (sfi->scope_info() != heap->empty_fixed_array()) {
      heap->RecordFixedArraySubTypeStats(
          SCOPE_INFO_SUB_TYPE, FixedArray::cast(sfi->scope_info())->Size());
    }
    ObjectStatsVisitBase(kVisitSharedFunctionInfo, map, obj);
  }
};


template <>
class MarkCompactMarkingVisitor::ObjectStatsTracker<
    MarkCompactMarkingVisitor::kVisitFixedArray> {
 public:
  static inline void Visit(Map* map, HeapObject* obj) {
    Heap* heap = map->GetHeap();
    FixedArray* fixed_array = FixedArray::cast(obj);
    if (fixed_array == heap->string_table()) {
      heap->RecordFixedArraySubTypeStats(STRING_TABLE_SUB_TYPE,
                                         fixed_array->Size());
    }
    ObjectStatsVisitBase(kVisitFixedArray, map, obj);
  }
};


void MarkCompactMarkingVisitor::Initialize() {
  StaticMarkingVisitor<MarkCompactMarkingVisitor>::Initialize();

  table_.Register(kVisitJSRegExp, &VisitRegExpAndFlushCode);

  if (FLAG_track_gc_object_stats) {
    // Copy the visitor table to make call-through possible.
    non_count_table_.CopyFrom(&table_);
#define VISITOR_ID_COUNT_FUNCTION(id) \
  table_.Register(kVisit##id, ObjectStatsTracker<kVisit##id>::Visit);
    VISITOR_ID_LIST(VISITOR_ID_COUNT_FUNCTION)
#undef VISITOR_ID_COUNT_FUNCTION
  }
}


VisitorDispatchTable<MarkCompactMarkingVisitor::Callback>
    MarkCompactMarkingVisitor::non_count_table_;


class CodeMarkingVisitor : public ThreadVisitor {
 public:
  explicit CodeMarkingVisitor(MarkCompactCollector* collector)
      : collector_(collector) {}

  void VisitThread(Isolate* isolate, ThreadLocalTop* top) {
    collector_->PrepareThreadForCodeFlushing(isolate, top);
  }

 private:
  MarkCompactCollector* collector_;
};


class SharedFunctionInfoMarkingVisitor : public ObjectVisitor {
 public:
  explicit SharedFunctionInfoMarkingVisitor(MarkCompactCollector* collector)
      : collector_(collector) {}

  void VisitPointers(Object** start, Object** end) {
    for (Object** p = start; p < end; p++) VisitPointer(p);
  }

  void VisitPointer(Object** slot) {
    Object* obj = *slot;
    if (obj->IsSharedFunctionInfo()) {
      SharedFunctionInfo* shared = reinterpret_cast<SharedFunctionInfo*>(obj);
      MarkBit shared_mark = Marking::MarkBitFrom(shared);
      MarkBit code_mark = Marking::MarkBitFrom(shared->code());
      collector_->MarkObject(shared->code(), code_mark);
      collector_->MarkObject(shared, shared_mark);
    }
  }

 private:
  MarkCompactCollector* collector_;
};


void MarkCompactCollector::PrepareThreadForCodeFlushing(Isolate* isolate,
                                                        ThreadLocalTop* top) {
  for (StackFrameIterator it(isolate, top); !it.done(); it.Advance()) {
    // Note: for the frame that has a pending lazy deoptimization
    // StackFrame::unchecked_code will return a non-optimized code object for
    // the outermost function and StackFrame::LookupCode will return
    // actual optimized code object.
    StackFrame* frame = it.frame();
    Code* code = frame->unchecked_code();
    MarkBit code_mark = Marking::MarkBitFrom(code);
    MarkObject(code, code_mark);
    if (frame->is_optimized()) {
      MarkCompactMarkingVisitor::MarkInlinedFunctionsCode(heap(),
                                                          frame->LookupCode());
    }
  }
}


void MarkCompactCollector::PrepareForCodeFlushing() {
  // Enable code flushing for non-incremental cycles.
  if (FLAG_flush_code && !FLAG_flush_code_incrementally) {
    EnableCodeFlushing(!was_marked_incrementally_);
  }

  // If code flushing is disabled, there is no need to prepare for it.
  if (!is_code_flushing_enabled()) return;

  // Ensure that empty descriptor array is marked. Method MarkDescriptorArray
  // relies on it being marked before any other descriptor array.
  HeapObject* descriptor_array = heap()->empty_descriptor_array();
  MarkBit descriptor_array_mark = Marking::MarkBitFrom(descriptor_array);
  MarkObject(descriptor_array, descriptor_array_mark);

  // Make sure we are not referencing the code from the stack.
  DCHECK(this == heap()->mark_compact_collector());
  PrepareThreadForCodeFlushing(heap()->isolate(),
                               heap()->isolate()->thread_local_top());

  // Iterate the archived stacks in all threads to check if
  // the code is referenced.
  CodeMarkingVisitor code_marking_visitor(this);
  heap()->isolate()->thread_manager()->IterateArchivedThreads(
      &code_marking_visitor);

  SharedFunctionInfoMarkingVisitor visitor(this);
  heap()->isolate()->compilation_cache()->IterateFunctions(&visitor);
  heap()->isolate()->handle_scope_implementer()->Iterate(&visitor);

  ProcessMarkingDeque();
}


// Visitor class for marking heap roots.
class RootMarkingVisitor : public ObjectVisitor {
 public:
  explicit RootMarkingVisitor(Heap* heap)
      : collector_(heap->mark_compact_collector()) {}

  void VisitPointer(Object** p) { MarkObjectByPointer(p); }

  void VisitPointers(Object** start, Object** end) {
    for (Object** p = start; p < end; p++) MarkObjectByPointer(p);
  }

  // Skip the weak next code link in a code object, which is visited in
  // ProcessTopOptimizedFrame.
  void VisitNextCodeLink(Object** p) {}

 private:
  void MarkObjectByPointer(Object** p) {
    if (!(*p)->IsHeapObject()) return;

    // Replace flat cons strings in place.
    HeapObject* object = ShortCircuitConsString(p);
    MarkBit mark_bit = Marking::MarkBitFrom(object);
    if (mark_bit.Get()) return;

    Map* map = object->map();
    // Mark the object.
    collector_->SetMark(object, mark_bit);

    // Mark the map pointer and body, and push them on the marking stack.
    MarkBit map_mark = Marking::MarkBitFrom(map);
    collector_->MarkObject(map, map_mark);
    MarkCompactMarkingVisitor::IterateBody(map, object);

    // Mark all the objects reachable from the map and body.  May leave
    // overflowed objects in the heap.
    collector_->EmptyMarkingDeque();
  }

  MarkCompactCollector* collector_;
};


// Helper class for pruning the string table.
template <bool finalize_external_strings>
class StringTableCleaner : public ObjectVisitor {
 public:
  explicit StringTableCleaner(Heap* heap) : heap_(heap), pointers_removed_(0) {}

  virtual void VisitPointers(Object** start, Object** end) {
    // Visit all HeapObject pointers in [start, end).
    for (Object** p = start; p < end; p++) {
      Object* o = *p;
      if (o->IsHeapObject() &&
          !Marking::MarkBitFrom(HeapObject::cast(o)).Get()) {
        if (finalize_external_strings) {
          DCHECK(o->IsExternalString());
          heap_->FinalizeExternalString(String::cast(*p));
        } else {
          pointers_removed_++;
        }
        // Set the entry to the_hole_value (as deleted).
        *p = heap_->the_hole_value();
      }
    }
  }

  int PointersRemoved() {
    DCHECK(!finalize_external_strings);
    return pointers_removed_;
  }

 private:
  Heap* heap_;
  int pointers_removed_;
};


typedef StringTableCleaner<false> InternalizedStringTableCleaner;
typedef StringTableCleaner<true> ExternalStringTableCleaner;


// Implementation of WeakObjectRetainer for mark compact GCs. All marked objects
// are retained.
class MarkCompactWeakObjectRetainer : public WeakObjectRetainer {
 public:
  virtual Object* RetainAs(Object* object) {
    if (Marking::MarkBitFrom(HeapObject::cast(object)).Get()) {
      return object;
    } else if (object->IsAllocationSite() &&
               !(AllocationSite::cast(object)->IsZombie())) {
      // "dead" AllocationSites need to live long enough for a traversal of new
      // space. These sites get a one-time reprieve.
      AllocationSite* site = AllocationSite::cast(object);
      site->MarkZombie();
      site->GetHeap()->mark_compact_collector()->MarkAllocationSite(site);
      return object;
    } else {
      return NULL;
    }
  }
};


// Fill the marking stack with overflowed objects returned by the given
// iterator.  Stop when the marking stack is filled or the end of the space
// is reached, whichever comes first.
template <class T>
static void DiscoverGreyObjectsWithIterator(Heap* heap,
                                            MarkingDeque* marking_deque,
                                            T* it) {
  // The caller should ensure that the marking stack is initially not full,
  // so that we don't waste effort pointlessly scanning for objects.
  DCHECK(!marking_deque->IsFull());

  Map* filler_map = heap->one_pointer_filler_map();
  for (HeapObject* object = it->Next(); object != NULL; object = it->Next()) {
    MarkBit markbit = Marking::MarkBitFrom(object);
    if ((object->map() != filler_map) && Marking::IsGrey(markbit)) {
      Marking::GreyToBlack(markbit);
      MemoryChunk::IncrementLiveBytesFromGC(object->address(), object->Size());
      marking_deque->PushBlack(object);
      if (marking_deque->IsFull()) return;
    }
  }
}


static inline int MarkWordToObjectStarts(uint32_t mark_bits, int* starts);


static void DiscoverGreyObjectsOnPage(MarkingDeque* marking_deque,
                                      MemoryChunk* p) {
  DCHECK(!marking_deque->IsFull());
  DCHECK(strcmp(Marking::kWhiteBitPattern, "00") == 0);
  DCHECK(strcmp(Marking::kBlackBitPattern, "10") == 0);
  DCHECK(strcmp(Marking::kGreyBitPattern, "11") == 0);
  DCHECK(strcmp(Marking::kImpossibleBitPattern, "01") == 0);

  for (MarkBitCellIterator it(p); !it.Done(); it.Advance()) {
    Address cell_base = it.CurrentCellBase();
    MarkBit::CellType* cell = it.CurrentCell();

    const MarkBit::CellType current_cell = *cell;
    if (current_cell == 0) continue;

    MarkBit::CellType grey_objects;
    if (it.HasNext()) {
      const MarkBit::CellType next_cell = *(cell + 1);
      grey_objects = current_cell & ((current_cell >> 1) |
                                     (next_cell << (Bitmap::kBitsPerCell - 1)));
    } else {
      grey_objects = current_cell & (current_cell >> 1);
    }

    int offset = 0;
    while (grey_objects != 0) {
      int trailing_zeros = base::bits::CountTrailingZeros32(grey_objects);
      grey_objects >>= trailing_zeros;
      offset += trailing_zeros;
      MarkBit markbit(cell, 1 << offset, false);
      DCHECK(Marking::IsGrey(markbit));
      Marking::GreyToBlack(markbit);
      Address addr = cell_base + offset * kPointerSize;
      HeapObject* object = HeapObject::FromAddress(addr);
      MemoryChunk::IncrementLiveBytesFromGC(object->address(), object->Size());
      marking_deque->PushBlack(object);
      if (marking_deque->IsFull()) return;
      offset += 2;
      grey_objects >>= 2;
    }

    grey_objects >>= (Bitmap::kBitsPerCell - 1);
  }
}


int MarkCompactCollector::DiscoverAndEvacuateBlackObjectsOnPage(
    NewSpace* new_space, NewSpacePage* p) {
  DCHECK(strcmp(Marking::kWhiteBitPattern, "00") == 0);
  DCHECK(strcmp(Marking::kBlackBitPattern, "10") == 0);
  DCHECK(strcmp(Marking::kGreyBitPattern, "11") == 0);
  DCHECK(strcmp(Marking::kImpossibleBitPattern, "01") == 0);

  MarkBit::CellType* cells = p->markbits()->cells();
  int survivors_size = 0;

  for (MarkBitCellIterator it(p); !it.Done(); it.Advance()) {
    Address cell_base = it.CurrentCellBase();
    MarkBit::CellType* cell = it.CurrentCell();

    MarkBit::CellType current_cell = *cell;
    if (current_cell == 0) continue;

    int offset = 0;
    while (current_cell != 0) {
      int trailing_zeros = base::bits::CountTrailingZeros32(current_cell);
      current_cell >>= trailing_zeros;
      offset += trailing_zeros;
      Address address = cell_base + offset * kPointerSize;
      HeapObject* object = HeapObject::FromAddress(address);

      int size = object->Size();
      survivors_size += size;

      Heap::UpdateAllocationSiteFeedback(object, Heap::RECORD_SCRATCHPAD_SLOT);

      offset++;
      current_cell >>= 1;

      // TODO(hpayer): Refactor EvacuateObject and call this function instead.
      if (heap()->ShouldBePromoted(object->address(), size)) {
        if (!TryPromoteObject(object, size)) {
          V8::FatalProcessOutOfMemory("Full GC promotion failed");
        }
      } else {
        AllocationResult allocation = new_space->AllocateRaw(size);
        if (allocation.IsRetry()) {
          if (!new_space->AddFreshPage()) {
            // Shouldn't happen. We are sweeping linearly, and to-space
            // has the same number of pages as from-space, so there is
            // always room.
            UNREACHABLE();
          }
          allocation = new_space->AllocateRaw(size);
          DCHECK(!allocation.IsRetry());
        }
        Object* target = allocation.ToObjectChecked();

        MigrateObject(HeapObject::cast(target), object, size, NEW_SPACE);
        heap()->IncrementSemiSpaceCopiedObjectSize(size);
      }
    }
    *cells = 0;
  }
  return survivors_size;
}


static void DiscoverGreyObjectsInSpace(Heap* heap, MarkingDeque* marking_deque,
                                       PagedSpace* space) {
  PageIterator it(space);
  while (it.has_next()) {
    Page* p = it.next();
    DiscoverGreyObjectsOnPage(marking_deque, p);
    if (marking_deque->IsFull()) return;
  }
}


static void DiscoverGreyObjectsInNewSpace(Heap* heap,
                                          MarkingDeque* marking_deque) {
  NewSpace* space = heap->new_space();
  NewSpacePageIterator it(space->bottom(), space->top());
  while (it.has_next()) {
    NewSpacePage* page = it.next();
    DiscoverGreyObjectsOnPage(marking_deque, page);
    if (marking_deque->IsFull()) return;
  }
}


bool MarkCompactCollector::IsUnmarkedHeapObject(Object** p) {
  Object* o = *p;
  if (!o->IsHeapObject()) return false;
  HeapObject* heap_object = HeapObject::cast(o);
  MarkBit mark = Marking::MarkBitFrom(heap_object);
  return !mark.Get();
}


bool MarkCompactCollector::IsUnmarkedHeapObjectWithHeap(Heap* heap,
                                                        Object** p) {
  Object* o = *p;
  DCHECK(o->IsHeapObject());
  HeapObject* heap_object = HeapObject::cast(o);
  MarkBit mark = Marking::MarkBitFrom(heap_object);
  return !mark.Get();
}


void MarkCompactCollector::MarkStringTable(RootMarkingVisitor* visitor) {
  StringTable* string_table = heap()->string_table();
  // Mark the string table itself.
  MarkBit string_table_mark = Marking::MarkBitFrom(string_table);
  if (!string_table_mark.Get()) {
    // String table could have already been marked by visiting the handles list.
    SetMark(string_table, string_table_mark);
  }
  // Explicitly mark the prefix.
  string_table->IteratePrefix(visitor);
  ProcessMarkingDeque();
}


void MarkCompactCollector::MarkAllocationSite(AllocationSite* site) {
  MarkBit mark_bit = Marking::MarkBitFrom(site);
  SetMark(site, mark_bit);
}


void MarkCompactCollector::MarkRoots(RootMarkingVisitor* visitor) {
  // Mark the heap roots including global variables, stack variables,
  // etc., and all objects reachable from them.
  heap()->IterateStrongRoots(visitor, VISIT_ONLY_STRONG);

  // Handle the string table specially.
  MarkStringTable(visitor);

  // There may be overflowed objects in the heap.  Visit them now.
  while (marking_deque_.overflowed()) {
    RefillMarkingDeque();
    EmptyMarkingDeque();
  }
}


void MarkCompactCollector::MarkImplicitRefGroups(
    MarkObjectFunction mark_object) {
  List<ImplicitRefGroup*>* ref_groups =
      isolate()->global_handles()->implicit_ref_groups();

  int last = 0;
  for (int i = 0; i < ref_groups->length(); i++) {
    ImplicitRefGroup* entry = ref_groups->at(i);
    DCHECK(entry != NULL);

    if (!IsMarked(*entry->parent)) {
      (*ref_groups)[last++] = entry;
      continue;
    }

    Object*** children = entry->children;
    // A parent object is marked, so mark all child heap objects.
    for (size_t j = 0; j < entry->length; ++j) {
      if ((*children[j])->IsHeapObject()) {
        mark_object(heap(), HeapObject::cast(*children[j]));
      }
    }

    // Once the entire group has been marked, dispose it because it's
    // not needed anymore.
    delete entry;
  }
  ref_groups->Rewind(last);
}


// Mark all objects reachable from the objects on the marking stack.
// Before: the marking stack contains zero or more heap object pointers.
// After: the marking stack is empty, and all objects reachable from the
// marking stack have been marked, or are overflowed in the heap.
void MarkCompactCollector::EmptyMarkingDeque() {
  Map* filler_map = heap_->one_pointer_filler_map();
  while (!marking_deque_.IsEmpty()) {
    HeapObject* object = marking_deque_.Pop();
    // Explicitly skip one word fillers. Incremental markbit patterns are
    // correct only for objects that occupy at least two words.
    Map* map = object->map();
    if (map == filler_map) continue;

    DCHECK(object->IsHeapObject());
    DCHECK(heap()->Contains(object));
    DCHECK(!Marking::IsWhite(Marking::MarkBitFrom(object)));

    MarkBit map_mark = Marking::MarkBitFrom(map);
    MarkObject(map, map_mark);

    MarkCompactMarkingVisitor::IterateBody(map, object);
  }
}


// Sweep the heap for overflowed objects, clear their overflow bits, and
// push them on the marking stack.  Stop early if the marking stack fills
// before sweeping completes.  If sweeping completes, there are no remaining
// overflowed objects in the heap so the overflow flag on the markings stack
// is cleared.
void MarkCompactCollector::RefillMarkingDeque() {
  DCHECK(marking_deque_.overflowed());

  DiscoverGreyObjectsInNewSpace(heap(), &marking_deque_);
  if (marking_deque_.IsFull()) return;

  DiscoverGreyObjectsInSpace(heap(), &marking_deque_,
                             heap()->old_pointer_space());
  if (marking_deque_.IsFull()) return;

  DiscoverGreyObjectsInSpace(heap(), &marking_deque_, heap()->old_data_space());
  if (marking_deque_.IsFull()) return;

  DiscoverGreyObjectsInSpace(heap(), &marking_deque_, heap()->code_space());
  if (marking_deque_.IsFull()) return;

  DiscoverGreyObjectsInSpace(heap(), &marking_deque_, heap()->map_space());
  if (marking_deque_.IsFull()) return;

  DiscoverGreyObjectsInSpace(heap(), &marking_deque_, heap()->cell_space());
  if (marking_deque_.IsFull()) return;

  LargeObjectIterator lo_it(heap()->lo_space());
  DiscoverGreyObjectsWithIterator(heap(), &marking_deque_, &lo_it);
  if (marking_deque_.IsFull()) return;

  marking_deque_.ClearOverflowed();
}


// Mark all objects reachable (transitively) from objects on the marking
// stack.  Before: the marking stack contains zero or more heap object
// pointers.  After: the marking stack is empty and there are no overflowed
// objects in the heap.
void MarkCompactCollector::ProcessMarkingDeque() {
  EmptyMarkingDeque();
  while (marking_deque_.overflowed()) {
    RefillMarkingDeque();
    EmptyMarkingDeque();
  }
}


// Mark all objects reachable (transitively) from objects on the marking
// stack including references only considered in the atomic marking pause.
void MarkCompactCollector::ProcessEphemeralMarking(
    ObjectVisitor* visitor, bool only_process_harmony_weak_collections) {
  bool work_to_do = true;
  DCHECK(marking_deque_.IsEmpty() && !marking_deque_.overflowed());
  while (work_to_do) {
    if (!only_process_harmony_weak_collections) {
      isolate()->global_handles()->IterateObjectGroups(
          visitor, &IsUnmarkedHeapObjectWithHeap);
      MarkImplicitRefGroups(&MarkCompactMarkingVisitor::MarkObject);
    }
    ProcessWeakCollections();
    work_to_do = !marking_deque_.IsEmpty();
    ProcessMarkingDeque();
  }
}


void MarkCompactCollector::ProcessTopOptimizedFrame(ObjectVisitor* visitor) {
  for (StackFrameIterator it(isolate(), isolate()->thread_local_top());
       !it.done(); it.Advance()) {
    if (it.frame()->type() == StackFrame::JAVA_SCRIPT) {
      return;
    }
    if (it.frame()->type() == StackFrame::OPTIMIZED) {
      Code* code = it.frame()->LookupCode();
      if (!code->CanDeoptAt(it.frame()->pc())) {
        code->CodeIterateBody(visitor);
      }
      ProcessMarkingDeque();
      return;
    }
  }
}


void MarkCompactCollector::RetainMaps() {
  if (reduce_memory_footprint_ || abort_incremental_marking_ ||
      FLAG_retain_maps_for_n_gc == 0) {
    // Do not retain dead maps if flag disables it or there is
    // - memory pressure (reduce_memory_footprint_),
    // - GC is requested by tests or dev-tools (abort_incremental_marking_).
    return;
  }

  ArrayList* retained_maps = heap()->retained_maps();
  int length = retained_maps->Length();
  int new_length = 0;
  for (int i = 0; i < length; i += 2) {
    DCHECK(retained_maps->Get(i)->IsWeakCell());
    WeakCell* cell = WeakCell::cast(retained_maps->Get(i));
    if (cell->cleared()) continue;
    int age = Smi::cast(retained_maps->Get(i + 1))->value();
    int new_age;
    Map* map = Map::cast(cell->value());
    MarkBit map_mark = Marking::MarkBitFrom(map);
    if (!map_mark.Get()) {
      if (age == 0) {
        // The map has aged. Do not retain this map.
        continue;
      }
      Object* constructor = map->GetConstructor();
      if (!constructor->IsHeapObject() ||
          !Marking::MarkBitFrom(HeapObject::cast(constructor)).Get()) {
        // The constructor is dead, no new objects with this map can
        // be created. Do not retain this map.
        continue;
      }
      Object* prototype = map->prototype();
      if (prototype->IsHeapObject() &&
          !Marking::MarkBitFrom(HeapObject::cast(prototype)).Get()) {
        // The prototype is not marked, age the map.
        new_age = age - 1;
      } else {
        // The prototype and the constructor are marked, this map keeps only
        // transition tree alive, not JSObjects. Do not age the map.
        new_age = age;
      }
      MarkObject(map, map_mark);
    } else {
      new_age = FLAG_retain_maps_for_n_gc;
    }
    if (i != new_length) {
      retained_maps->Set(new_length, cell);
      Object** slot = retained_maps->Slot(new_length);
      RecordSlot(slot, slot, cell);
      retained_maps->Set(new_length + 1, Smi::FromInt(new_age));
    } else if (new_age != age) {
      retained_maps->Set(new_length + 1, Smi::FromInt(new_age));
    }
    new_length += 2;
  }
  Object* undefined = heap()->undefined_value();
  for (int i = new_length; i < length; i++) {
    retained_maps->Clear(i, undefined);
  }
  if (new_length != length) retained_maps->SetLength(new_length);
  ProcessMarkingDeque();
}


void MarkCompactCollector::EnsureMarkingDequeIsCommittedAndInitialize() {
  if (marking_deque_memory_ == NULL) {
    marking_deque_memory_ = new base::VirtualMemory(4 * MB);
  }
  if (!marking_deque_memory_committed_) {
    if (!marking_deque_memory_->Commit(
            reinterpret_cast<Address>(marking_deque_memory_->address()),
            marking_deque_memory_->size(),
            false)) {  // Not executable.
      V8::FatalProcessOutOfMemory("EnsureMarkingDequeIsCommitted");
    }
    marking_deque_memory_committed_ = true;
    InitializeMarkingDeque();
  }
}


void MarkCompactCollector::InitializeMarkingDeque() {
  if (marking_deque_memory_committed_) {
    Address addr = static_cast<Address>(marking_deque_memory_->address());
    size_t size = marking_deque_memory_->size();
    if (FLAG_force_marking_deque_overflows) size = 64 * kPointerSize;
    marking_deque_.Initialize(addr, addr + size);
  }
}


void MarkCompactCollector::UncommitMarkingDeque() {
  if (marking_deque_memory_committed_) {
    bool success = marking_deque_memory_->Uncommit(
        reinterpret_cast<Address>(marking_deque_memory_->address()),
        marking_deque_memory_->size());
    CHECK(success);
    marking_deque_memory_committed_ = false;
  }
}


void MarkCompactCollector::MarkLiveObjects() {
  GCTracer::Scope gc_scope(heap()->tracer(), GCTracer::Scope::MC_MARK);
  double start_time = 0.0;
  if (FLAG_print_cumulative_gc_stat) {
    start_time = base::OS::TimeCurrentMillis();
  }
  // The recursive GC marker detects when it is nearing stack overflow,
  // and switches to a different marking system.  JS interrupts interfere
  // with the C stack limit check.
  PostponeInterruptsScope postpone(isolate());

  IncrementalMarking* incremental_marking = heap_->incremental_marking();
  if (was_marked_incrementally_) {
    incremental_marking->Finalize();
  } else {
    // Abort any pending incremental activities e.g. incremental sweeping.
    incremental_marking->Abort();
    InitializeMarkingDeque();
  }

#ifdef DEBUG
  DCHECK(state_ == PREPARE_GC);
  state_ = MARK_LIVE_OBJECTS;
#endif

  EnsureMarkingDequeIsCommittedAndInitialize();

  PrepareForCodeFlushing();

  if (was_marked_incrementally_) {
    // There is no write barrier on cells so we have to scan them now at the end
    // of the incremental marking.
    {
      HeapObjectIterator cell_iterator(heap()->cell_space());
      HeapObject* cell;
      while ((cell = cell_iterator.Next()) != NULL) {
        DCHECK(cell->IsCell());
        if (IsMarked(cell)) {
          int offset = Cell::kValueOffset;
          MarkCompactMarkingVisitor::VisitPointer(
              heap(), reinterpret_cast<Object**>(cell->address() + offset));
        }
      }
    }
  }

  RootMarkingVisitor root_visitor(heap());
  MarkRoots(&root_visitor);

  ProcessTopOptimizedFrame(&root_visitor);

  // Retaining dying maps should happen before or during ephemeral marking
  // because a map could keep the key of an ephemeron alive. Note that map
  // aging is imprecise: maps that are kept alive only by ephemerons will age.
  RetainMaps();

  {
    GCTracer::Scope gc_scope(heap()->tracer(), GCTracer::Scope::MC_WEAKCLOSURE);

    // The objects reachable from the roots are marked, yet unreachable
    // objects are unmarked.  Mark objects reachable due to host
    // application specific logic or through Harmony weak maps.
    ProcessEphemeralMarking(&root_visitor, false);

    // The objects reachable from the roots, weak maps or object groups
    // are marked. Objects pointed to only by weak global handles cannot be
    // immediately reclaimed. Instead, we have to mark them as pending and mark
    // objects reachable from them.
    //
    // First we identify nonlive weak handles and mark them as pending
    // destruction.
    heap()->isolate()->global_handles()->IdentifyWeakHandles(
        &IsUnmarkedHeapObject);
    // Then we mark the objects.
    heap()->isolate()->global_handles()->IterateWeakRoots(&root_visitor);
    ProcessMarkingDeque();

    // Repeat Harmony weak maps marking to mark unmarked objects reachable from
    // the weak roots we just marked as pending destruction.
    //
    // We only process harmony collections, as all object groups have been fully
    // processed and no weakly reachable node can discover new objects groups.
    ProcessEphemeralMarking(&root_visitor, true);
  }

  AfterMarking();

  if (FLAG_print_cumulative_gc_stat) {
    heap_->tracer()->AddMarkingTime(base::OS::TimeCurrentMillis() - start_time);
  }
}


void MarkCompactCollector::AfterMarking() {
  // Prune the string table removing all strings only pointed to by the
  // string table.  Cannot use string_table() here because the string
  // table is marked.
  StringTable* string_table = heap()->string_table();
  InternalizedStringTableCleaner internalized_visitor(heap());
  string_table->IterateElements(&internalized_visitor);
  string_table->ElementsRemoved(internalized_visitor.PointersRemoved());

  ExternalStringTableCleaner external_visitor(heap());
  heap()->external_string_table_.Iterate(&external_visitor);
  heap()->external_string_table_.CleanUp();

  // Process the weak references.
  MarkCompactWeakObjectRetainer mark_compact_object_retainer;
  heap()->ProcessAllWeakReferences(&mark_compact_object_retainer);

  // Remove object groups after marking phase.
  heap()->isolate()->global_handles()->RemoveObjectGroups();
  heap()->isolate()->global_handles()->RemoveImplicitRefGroups();

  // Flush code from collected candidates.
  if (is_code_flushing_enabled()) {
    code_flusher_->ProcessCandidates();
    // If incremental marker does not support code flushing, we need to
    // disable it before incremental marking steps for next cycle.
    if (FLAG_flush_code && !FLAG_flush_code_incrementally) {
      EnableCodeFlushing(false);
    }
  }

  if (FLAG_track_gc_object_stats) {
    heap()->CheckpointObjectStats();
  }
}


void MarkCompactCollector::ClearNonLiveReferences() {
  // Iterate over the map space, setting map transitions that go from
  // a marked map to an unmarked map to null transitions.  This action
  // is carried out only on maps of JSObjects and related subtypes.
  HeapObjectIterator map_iterator(heap()->map_space());
  for (HeapObject* obj = map_iterator.Next(); obj != NULL;
       obj = map_iterator.Next()) {
    Map* map = Map::cast(obj);

    if (!map->CanTransition()) continue;

    MarkBit map_mark = Marking::MarkBitFrom(map);
    ClearNonLivePrototypeTransitions(map);
    ClearNonLiveMapTransitions(map, map_mark);

    if (!map_mark.Get()) {
      have_code_to_deoptimize_ |=
          map->dependent_code()->MarkCodeForDeoptimization(
              isolate(), DependentCode::kWeakCodeGroup);
      map->set_dependent_code(DependentCode::cast(heap()->empty_fixed_array()));
    }
  }

  WeakHashTable* table = heap_->weak_object_to_code_table();
  uint32_t capacity = table->Capacity();
  for (uint32_t i = 0; i < capacity; i++) {
    uint32_t key_index = table->EntryToIndex(i);
    Object* key = table->get(key_index);
    if (!table->IsKey(key)) continue;
    uint32_t value_index = table->EntryToValueIndex(i);
    Object* value = table->get(value_index);
    DCHECK(key->IsWeakCell());
    if (WeakCell::cast(key)->cleared()) {
      have_code_to_deoptimize_ |=
          DependentCode::cast(value)->MarkCodeForDeoptimization(
              isolate(), DependentCode::kWeakCodeGroup);
      table->set(key_index, heap_->the_hole_value());
      table->set(value_index, heap_->the_hole_value());
      table->ElementRemoved();
    }
  }
}


void MarkCompactCollector::ClearNonLivePrototypeTransitions(Map* map) {
  FixedArray* prototype_transitions =
      TransitionArray::GetPrototypeTransitions(map);
  int number_of_transitions =
      TransitionArray::NumberOfPrototypeTransitions(prototype_transitions);

  const int header = TransitionArray::kProtoTransitionHeaderSize;
  int new_number_of_transitions = 0;
  for (int i = 0; i < number_of_transitions; i++) {
    Object* cached_map = prototype_transitions->get(header + i);
    if (IsMarked(cached_map)) {
      if (new_number_of_transitions != i) {
        prototype_transitions->set(header + new_number_of_transitions,
                                   cached_map, SKIP_WRITE_BARRIER);
      }
      new_number_of_transitions++;
    }
  }

  if (new_number_of_transitions != number_of_transitions) {
    TransitionArray::SetNumberOfPrototypeTransitions(prototype_transitions,
                                                     new_number_of_transitions);
  }

  // Fill slots that became free with undefined value.
  for (int i = new_number_of_transitions; i < number_of_transitions; i++) {
    prototype_transitions->set_undefined(header + i);
  }
}


void MarkCompactCollector::ClearNonLiveMapTransitions(Map* map,
                                                      MarkBit map_mark) {
  Object* potential_parent = map->GetBackPointer();
  if (!potential_parent->IsMap()) return;
  Map* parent = Map::cast(potential_parent);

  // Follow back pointer, check whether we are dealing with a map transition
  // from a live map to a dead path and in case clear transitions of parent.
  bool current_is_alive = map_mark.Get();
  bool parent_is_alive = Marking::MarkBitFrom(parent).Get();
  if (!current_is_alive && parent_is_alive) {
    ClearMapTransitions(parent, map);
  }
}


// Clear a possible back pointer in case the transition leads to a dead map.
// Return true in case a back pointer has been cleared and false otherwise.
bool MarkCompactCollector::ClearMapBackPointer(Map* target) {
  if (Marking::MarkBitFrom(target).Get()) return false;
  target->SetBackPointer(heap_->undefined_value(), SKIP_WRITE_BARRIER);
  return true;
}


void MarkCompactCollector::ClearMapTransitions(Map* map, Map* dead_transition) {
  Object* transitions = map->raw_transitions();
  int num_transitions = TransitionArray::NumberOfTransitions(transitions);

  int number_of_own_descriptors = map->NumberOfOwnDescriptors();
  DescriptorArray* descriptors = map->instance_descriptors();

  // A previously existing simple transition (stored in a WeakCell) may have
  // been cleared. Clear the useless cell pointer, and take ownership
  // of the descriptor array.
  if (transitions->IsWeakCell() && WeakCell::cast(transitions)->cleared()) {
    map->set_raw_transitions(Smi::FromInt(0));
  }
  if (num_transitions == 0 &&
      descriptors == dead_transition->instance_descriptors() &&
      number_of_own_descriptors > 0) {
    TrimDescriptorArray(map, descriptors, number_of_own_descriptors);
    DCHECK(descriptors->number_of_descriptors() == number_of_own_descriptors);
    map->set_owns_descriptors(true);
    return;
  }

  int transition_index = 0;

  bool descriptors_owner_died = false;

  // Compact all live descriptors to the left.
  for (int i = 0; i < num_transitions; ++i) {
    Map* target = TransitionArray::GetTarget(transitions, i);
    if (ClearMapBackPointer(target)) {
      if (target->instance_descriptors() == descriptors) {
        descriptors_owner_died = true;
      }
    } else {
      if (i != transition_index) {
        DCHECK(TransitionArray::IsFullTransitionArray(transitions));
        TransitionArray* t = TransitionArray::cast(transitions);
        Name* key = t->GetKey(i);
        t->SetKey(transition_index, key);
        Object** key_slot = t->GetKeySlot(transition_index);
        RecordSlot(key_slot, key_slot, key);
        // Target slots do not need to be recorded since maps are not compacted.
        t->SetTarget(transition_index, t->GetTarget(i));
      }
      transition_index++;
    }
  }

  // If there are no transitions to be cleared, return.
  // TODO(verwaest) Should be an assert, otherwise back pointers are not
  // properly cleared.
  if (transition_index == num_transitions) return;

  if (descriptors_owner_died) {
    if (number_of_own_descriptors > 0) {
      TrimDescriptorArray(map, descriptors, number_of_own_descriptors);
      DCHECK(descriptors->number_of_descriptors() == number_of_own_descriptors);
      map->set_owns_descriptors(true);
    } else {
      DCHECK(descriptors == heap_->empty_descriptor_array());
    }
  }

  // Note that we never eliminate a transition array, though we might right-trim
  // such that number_of_transitions() == 0. If this assumption changes,
  // TransitionArray::Insert() will need to deal with the case that a transition
  // array disappeared during GC.
  int trim = TransitionArray::Capacity(transitions) - transition_index;
  if (trim > 0) {
    // Non-full-TransitionArray cases can never reach this point.
    DCHECK(TransitionArray::IsFullTransitionArray(transitions));
    TransitionArray* t = TransitionArray::cast(transitions);
    heap_->RightTrimFixedArray<Heap::FROM_GC>(
        t, trim * TransitionArray::kTransitionSize);
    t->SetNumberOfTransitions(transition_index);
    // The map still has a full transition array.
    DCHECK(TransitionArray::IsFullTransitionArray(map->raw_transitions()));
  }
}


void MarkCompactCollector::TrimDescriptorArray(Map* map,
                                               DescriptorArray* descriptors,
                                               int number_of_own_descriptors) {
  int number_of_descriptors = descriptors->number_of_descriptors_storage();
  int to_trim = number_of_descriptors - number_of_own_descriptors;
  if (to_trim == 0) return;

  heap_->RightTrimFixedArray<Heap::FROM_GC>(
      descriptors, to_trim * DescriptorArray::kDescriptorSize);
  descriptors->SetNumberOfDescriptors(number_of_own_descriptors);

  if (descriptors->HasEnumCache()) TrimEnumCache(map, descriptors);
  descriptors->Sort();
}


void MarkCompactCollector::TrimEnumCache(Map* map,
                                         DescriptorArray* descriptors) {
  int live_enum = map->EnumLength();
  if (live_enum == kInvalidEnumCacheSentinel) {
    live_enum = map->NumberOfDescribedProperties(OWN_DESCRIPTORS, DONT_ENUM);
  }
  if (live_enum == 0) return descriptors->ClearEnumCache();

  FixedArray* enum_cache = descriptors->GetEnumCache();

  int to_trim = enum_cache->length() - live_enum;
  if (to_trim <= 0) return;
  heap_->RightTrimFixedArray<Heap::FROM_GC>(descriptors->GetEnumCache(),
                                            to_trim);

  if (!descriptors->HasEnumIndicesCache()) return;
  FixedArray* enum_indices_cache = descriptors->GetEnumIndicesCache();
  heap_->RightTrimFixedArray<Heap::FROM_GC>(enum_indices_cache, to_trim);
}


void MarkCompactCollector::ProcessWeakCollections() {
  GCTracer::Scope gc_scope(heap()->tracer(),
                           GCTracer::Scope::MC_WEAKCOLLECTION_PROCESS);
  Object* weak_collection_obj = heap()->encountered_weak_collections();
  while (weak_collection_obj != Smi::FromInt(0)) {
    JSWeakCollection* weak_collection =
        reinterpret_cast<JSWeakCollection*>(weak_collection_obj);
    DCHECK(MarkCompactCollector::IsMarked(weak_collection));
    if (weak_collection->table()->IsHashTable()) {
      ObjectHashTable* table = ObjectHashTable::cast(weak_collection->table());
      Object** anchor = reinterpret_cast<Object**>(table->address());
      for (int i = 0; i < table->Capacity(); i++) {
        if (MarkCompactCollector::IsMarked(HeapObject::cast(table->KeyAt(i)))) {
          Object** key_slot =
              table->RawFieldOfElementAt(ObjectHashTable::EntryToIndex(i));
          RecordSlot(anchor, key_slot, *key_slot);
          Object** value_slot =
              table->RawFieldOfElementAt(ObjectHashTable::EntryToValueIndex(i));
          MarkCompactMarkingVisitor::MarkObjectByPointer(this, anchor,
                                                         value_slot);
        }
      }
    }
    weak_collection_obj = weak_collection->next();
  }
}


void MarkCompactCollector::ClearWeakCollections() {
  GCTracer::Scope gc_scope(heap()->tracer(),
                           GCTracer::Scope::MC_WEAKCOLLECTION_CLEAR);
  Object* weak_collection_obj = heap()->encountered_weak_collections();
  while (weak_collection_obj != Smi::FromInt(0)) {
    JSWeakCollection* weak_collection =
        reinterpret_cast<JSWeakCollection*>(weak_collection_obj);
    DCHECK(MarkCompactCollector::IsMarked(weak_collection));
    if (weak_collection->table()->IsHashTable()) {
      ObjectHashTable* table = ObjectHashTable::cast(weak_collection->table());
      for (int i = 0; i < table->Capacity(); i++) {
        HeapObject* key = HeapObject::cast(table->KeyAt(i));
        if (!MarkCompactCollector::IsMarked(key)) {
          table->RemoveEntry(i);
        }
      }
    }
    weak_collection_obj = weak_collection->next();
    weak_collection->set_next(heap()->undefined_value());
  }
  heap()->set_encountered_weak_collections(Smi::FromInt(0));
}


void MarkCompactCollector::AbortWeakCollections() {
  GCTracer::Scope gc_scope(heap()->tracer(),
                           GCTracer::Scope::MC_WEAKCOLLECTION_ABORT);
  Object* weak_collection_obj = heap()->encountered_weak_collections();
  while (weak_collection_obj != Smi::FromInt(0)) {
    JSWeakCollection* weak_collection =
        reinterpret_cast<JSWeakCollection*>(weak_collection_obj);
    weak_collection_obj = weak_collection->next();
    weak_collection->set_next(heap()->undefined_value());
  }
  heap()->set_encountered_weak_collections(Smi::FromInt(0));
}


void MarkCompactCollector::ProcessAndClearWeakCells() {
  HeapObject* undefined = heap()->undefined_value();
  Object* weak_cell_obj = heap()->encountered_weak_cells();
  while (weak_cell_obj != Smi::FromInt(0)) {
    WeakCell* weak_cell = reinterpret_cast<WeakCell*>(weak_cell_obj);
    // We do not insert cleared weak cells into the list, so the value
    // cannot be a Smi here.
    HeapObject* value = HeapObject::cast(weak_cell->value());
    if (!MarkCompactCollector::IsMarked(value)) {
      // Cells for new-space objects embedded in optimized code are wrapped in
      // WeakCell and put into Heap::weak_object_to_code_table.
      // Such cells do not have any strong references but we want to keep them
      // alive as long as the cell value is alive.
      // TODO(ulan): remove this once we remove Heap::weak_object_to_code_table.
      if (value->IsCell()) {
        Object* cell_value = Cell::cast(value)->value();
        if (cell_value->IsHeapObject() &&
            MarkCompactCollector::IsMarked(HeapObject::cast(cell_value))) {
          // Resurrect the cell.
          MarkBit mark = Marking::MarkBitFrom(value);
          SetMark(value, mark);
          Object** slot = HeapObject::RawField(value, Cell::kValueOffset);
          RecordSlot(slot, slot, *slot);
          slot = HeapObject::RawField(weak_cell, WeakCell::kValueOffset);
          RecordSlot(slot, slot, *slot);
        } else {
          weak_cell->clear();
        }
      } else {
        weak_cell->clear();
      }
    } else {
      Object** slot = HeapObject::RawField(weak_cell, WeakCell::kValueOffset);
      RecordSlot(slot, slot, *slot);
    }
    weak_cell_obj = weak_cell->next();
    weak_cell->set_next(undefined, SKIP_WRITE_BARRIER);
  }
  heap()->set_encountered_weak_cells(Smi::FromInt(0));
}


void MarkCompactCollector::AbortWeakCells() {
  Object* undefined = heap()->undefined_value();
  Object* weak_cell_obj = heap()->encountered_weak_cells();
  while (weak_cell_obj != Smi::FromInt(0)) {
    WeakCell* weak_cell = reinterpret_cast<WeakCell*>(weak_cell_obj);
    weak_cell_obj = weak_cell->next();
    weak_cell->set_next(undefined, SKIP_WRITE_BARRIER);
  }
  heap()->set_encountered_weak_cells(Smi::FromInt(0));
}


void MarkCompactCollector::RecordMigratedSlot(Object* value, Address slot) {
  if (heap_->InNewSpace(value)) {
    heap_->store_buffer()->Mark(slot);
  } else if (value->IsHeapObject() && IsOnEvacuationCandidate(value)) {
    SlotsBuffer::AddTo(&slots_buffer_allocator_, &migration_slots_buffer_,
                       reinterpret_cast<Object**>(slot),
                       SlotsBuffer::IGNORE_OVERFLOW);
  }
}


// We scavenge new space simultaneously with sweeping. This is done in two
// passes.
//
// The first pass migrates all alive objects from one semispace to another or
// promotes them to old space.  Forwarding address is written directly into
// first word of object without any encoding.  If object is dead we write
// NULL as a forwarding address.
//
// The second pass updates pointers to new space in all spaces.  It is possible
// to encounter pointers to dead new space objects during traversal of pointers
// to new space.  We should clear them to avoid encountering them during next
// pointer iteration.  This is an issue if the store buffer overflows and we
// have to scan the entire old space, including dead objects, looking for
// pointers to new space.
void MarkCompactCollector::MigrateObject(HeapObject* dst, HeapObject* src,
                                         int size, AllocationSpace dest) {
  Address dst_addr = dst->address();
  Address src_addr = src->address();
  DCHECK(heap()->AllowedToBeMigrated(src, dest));
  DCHECK(dest != LO_SPACE && size <= Page::kMaxRegularHeapObjectSize);
  if (dest == OLD_POINTER_SPACE) {
    Address src_slot = src_addr;
    Address dst_slot = dst_addr;
    DCHECK(IsAligned(size, kPointerSize));

    bool may_contain_raw_values = src->MayContainRawValues();
#if V8_DOUBLE_FIELDS_UNBOXING
    LayoutDescriptorHelper helper(src->map());
    bool has_only_tagged_fields = helper.all_fields_tagged();
#endif
    for (int remaining = size / kPointerSize; remaining > 0; remaining--) {
      Object* value = Memory::Object_at(src_slot);

      Memory::Object_at(dst_slot) = value;

#if V8_DOUBLE_FIELDS_UNBOXING
      if (!may_contain_raw_values &&
          (has_only_tagged_fields ||
           helper.IsTagged(static_cast<int>(src_slot - src_addr))))
#else
      if (!may_contain_raw_values)
#endif
      {
        RecordMigratedSlot(value, dst_slot);
      }

      src_slot += kPointerSize;
      dst_slot += kPointerSize;
    }

    if (compacting_ && dst->IsJSFunction()) {
      Address code_entry_slot = dst_addr + JSFunction::kCodeEntryOffset;
      Address code_entry = Memory::Address_at(code_entry_slot);

      if (Page::FromAddress(code_entry)->IsEvacuationCandidate()) {
        SlotsBuffer::AddTo(&slots_buffer_allocator_, &migration_slots_buffer_,
                           SlotsBuffer::CODE_ENTRY_SLOT, code_entry_slot,
                           SlotsBuffer::IGNORE_OVERFLOW);
      }
    } else if (dst->IsConstantPoolArray()) {
      // We special case ConstantPoolArrays since they could contain integers
      // value entries which look like tagged pointers.
      // TODO(mstarzinger): restructure this code to avoid this special-casing.
      ConstantPoolArray* array = ConstantPoolArray::cast(dst);
      ConstantPoolArray::Iterator code_iter(array, ConstantPoolArray::CODE_PTR);
      while (!code_iter.is_finished()) {
        Address code_entry_slot =
            dst_addr + array->OffsetOfElementAt(code_iter.next_index());
        Address code_entry = Memory::Address_at(code_entry_slot);

        if (Page::FromAddress(code_entry)->IsEvacuationCandidate()) {
          SlotsBuffer::AddTo(&slots_buffer_allocator_, &migration_slots_buffer_,
                             SlotsBuffer::CODE_ENTRY_SLOT, code_entry_slot,
                             SlotsBuffer::IGNORE_OVERFLOW);
        }
      }
      ConstantPoolArray::Iterator heap_iter(array, ConstantPoolArray::HEAP_PTR);
      while (!heap_iter.is_finished()) {
        Address heap_slot =
            dst_addr + array->OffsetOfElementAt(heap_iter.next_index());
        Object* value = Memory::Object_at(heap_slot);
        RecordMigratedSlot(value, heap_slot);
      }
    }
  } else if (dest == CODE_SPACE) {
    PROFILE(isolate(), CodeMoveEvent(src_addr, dst_addr));
    heap()->MoveBlock(dst_addr, src_addr, size);
    SlotsBuffer::AddTo(&slots_buffer_allocator_, &migration_slots_buffer_,
                       SlotsBuffer::RELOCATED_CODE_OBJECT, dst_addr,
                       SlotsBuffer::IGNORE_OVERFLOW);
    Code::cast(dst)->Relocate(dst_addr - src_addr);
  } else {
    DCHECK(dest == OLD_DATA_SPACE || dest == NEW_SPACE);
    heap()->MoveBlock(dst_addr, src_addr, size);
  }
  heap()->OnMoveEvent(dst, src, size);
  Memory::Address_at(src_addr) = dst_addr;
}


// Visitor for updating pointers from live objects in old spaces to new space.
// It does not expect to encounter pointers to dead objects.
class PointersUpdatingVisitor : public ObjectVisitor {
 public:
  explicit PointersUpdatingVisitor(Heap* heap) : heap_(heap) {}

  void VisitPointer(Object** p) { UpdatePointer(p); }

  void VisitPointers(Object** start, Object** end) {
    for (Object** p = start; p < end; p++) UpdatePointer(p);
  }

  void VisitEmbeddedPointer(RelocInfo* rinfo) {
    DCHECK(rinfo->rmode() == RelocInfo::EMBEDDED_OBJECT);
    Object* target = rinfo->target_object();
    Object* old_target = target;
    VisitPointer(&target);
    // Avoid unnecessary changes that might unnecessary flush the instruction
    // cache.
    if (target != old_target) {
      // TODO(jochen): Remove again after fixing http://crbug.com/452095
      CHECK(target->IsHeapObject() == old_target->IsHeapObject());
      rinfo->set_target_object(target);
    }
  }

  void VisitCodeTarget(RelocInfo* rinfo) {
    DCHECK(RelocInfo::IsCodeTarget(rinfo->rmode()));
    Object* target = Code::GetCodeFromTargetAddress(rinfo->target_address());
    Object* old_target = target;
    VisitPointer(&target);
    if (target != old_target) {
      // TODO(jochen): Remove again after fixing http://crbug.com/452095
      CHECK(target->IsHeapObject() == old_target->IsHeapObject());
      rinfo->set_target_address(Code::cast(target)->instruction_start());
    }
  }

  void VisitCodeAgeSequence(RelocInfo* rinfo) {
    DCHECK(RelocInfo::IsCodeAgeSequence(rinfo->rmode()));
    Object* stub = rinfo->code_age_stub();
    DCHECK(stub != NULL);
    VisitPointer(&stub);
    if (stub != rinfo->code_age_stub()) {
      // TODO(jochen): Remove again after fixing http://crbug.com/452095
      CHECK(stub->IsHeapObject() == rinfo->code_age_stub()->IsHeapObject());
      rinfo->set_code_age_stub(Code::cast(stub));
    }
  }

  void VisitDebugTarget(RelocInfo* rinfo) {
    DCHECK((RelocInfo::IsJSReturn(rinfo->rmode()) &&
            rinfo->IsPatchedReturnSequence()) ||
           (RelocInfo::IsDebugBreakSlot(rinfo->rmode()) &&
            rinfo->IsPatchedDebugBreakSlotSequence()));
    Object* target = Code::GetCodeFromTargetAddress(rinfo->call_address());
    VisitPointer(&target);
    // TODO(jochen): Remove again after fixing http://crbug.com/452095
    CHECK(target->IsCode() &&
          HAS_SMI_TAG(Code::cast(target)->instruction_start()));
    rinfo->set_call_address(Code::cast(target)->instruction_start());
  }

  static inline void UpdateSlot(Heap* heap, Object** slot) {
    Object* obj = reinterpret_cast<Object*>(
        base::NoBarrier_Load(reinterpret_cast<base::AtomicWord*>(slot)));

    if (!obj->IsHeapObject()) return;

    HeapObject* heap_obj = HeapObject::cast(obj);

// TODO(ishell): remove, once crbug/454297 is caught.
#if V8_TARGET_ARCH_64_BIT
#ifndef V8_OS_AIX  // no point checking on AIX as full 64 range is supported
    const uintptr_t kBoundary = V8_UINT64_C(1) << 48;
    STATIC_ASSERT(kBoundary > 0);
    if (reinterpret_cast<uintptr_t>(heap_obj->address()) >= kBoundary) {
      CheckLayoutDescriptorAndDie(heap, slot);
    }
#endif
#endif
    MapWord map_word = heap_obj->map_word();
    if (map_word.IsForwardingAddress()) {
      DCHECK(heap->InFromSpace(heap_obj) ||
             MarkCompactCollector::IsOnEvacuationCandidate(heap_obj));
      HeapObject* target = map_word.ToForwardingAddress();
      base::NoBarrier_CompareAndSwap(
          reinterpret_cast<base::AtomicWord*>(slot),
          reinterpret_cast<base::AtomicWord>(obj),
          reinterpret_cast<base::AtomicWord>(target));
      DCHECK(!heap->InFromSpace(target) &&
             !MarkCompactCollector::IsOnEvacuationCandidate(target));
    }
  }

 private:
  inline void UpdatePointer(Object** p) { UpdateSlot(heap_, p); }

  static void CheckLayoutDescriptorAndDie(Heap* heap, Object** slot);

  Heap* heap_;
};


#if V8_TARGET_ARCH_64_BIT
// TODO(ishell): remove, once crbug/454297 is caught.
void PointersUpdatingVisitor::CheckLayoutDescriptorAndDie(Heap* heap,
                                                          Object** slot) {
  const int kDataBufferSize = 128;
  uintptr_t data[kDataBufferSize] = {0};
  int index = 0;
  data[index++] = 0x10aaaaaaaaUL;  // begin marker

  data[index++] = reinterpret_cast<uintptr_t>(slot);
  data[index++] = 0x15aaaaaaaaUL;

  Address slot_address = reinterpret_cast<Address>(slot);

  uintptr_t space_owner_id = 0xb001;
  if (heap->new_space()->ToSpaceContains(slot_address)) {
    space_owner_id = 1;
  } else if (heap->new_space()->FromSpaceContains(slot_address)) {
    space_owner_id = 2;
  } else if (heap->old_pointer_space()->ContainsSafe(slot_address)) {
    space_owner_id = 3;
  } else if (heap->old_data_space()->ContainsSafe(slot_address)) {
    space_owner_id = 4;
  } else if (heap->code_space()->ContainsSafe(slot_address)) {
    space_owner_id = 5;
  } else if (heap->map_space()->ContainsSafe(slot_address)) {
    space_owner_id = 6;
  } else if (heap->cell_space()->ContainsSafe(slot_address)) {
    space_owner_id = 7;
  } else {
    // Lo space or other.
    space_owner_id = 8;
  }
  data[index++] = space_owner_id;
  data[index++] = 0x20aaaaaaaaUL;

  // Find map word lying near before the slot address (usually the map word is
  // at -3 words from the slot but just in case we look up further.
  Object** map_slot = slot;
  bool found = false;
  const int kMaxDistanceToMap = 64;
  for (int i = 0; i < kMaxDistanceToMap; i++, map_slot--) {
    Address map_address = reinterpret_cast<Address>(*map_slot);
    if (heap->map_space()->ContainsSafe(map_address)) {
      found = true;
      break;
    }
  }
  data[index++] = found;
  data[index++] = 0x30aaaaaaaaUL;
  data[index++] = reinterpret_cast<uintptr_t>(map_slot);
  data[index++] = 0x35aaaaaaaaUL;

  if (found) {
    Address obj_address = reinterpret_cast<Address>(map_slot);
    Address end_of_page =
        reinterpret_cast<Address>(Page::FromAddress(obj_address)) +
        Page::kPageSize;
    Address end_address =
        Min(obj_address + kPointerSize * kMaxDistanceToMap, end_of_page);
    int size = static_cast<int>(end_address - obj_address);
    data[index++] = size / kPointerSize;
    data[index++] = 0x40aaaaaaaaUL;
    memcpy(&data[index], reinterpret_cast<void*>(map_slot), size);
    index += size / kPointerSize;
    data[index++] = 0x50aaaaaaaaUL;

    HeapObject* object = HeapObject::FromAddress(obj_address);
    data[index++] = reinterpret_cast<uintptr_t>(object);
    data[index++] = 0x60aaaaaaaaUL;

    Map* map = object->map();
    data[index++] = reinterpret_cast<uintptr_t>(map);
    data[index++] = 0x70aaaaaaaaUL;

    LayoutDescriptor* layout_descriptor = map->layout_descriptor();
    data[index++] = reinterpret_cast<uintptr_t>(layout_descriptor);
    data[index++] = 0x80aaaaaaaaUL;

    memcpy(&data[index], reinterpret_cast<void*>(map->address()), Map::kSize);
    index += Map::kSize / kPointerSize;
    data[index++] = 0x90aaaaaaaaUL;
  }

  data[index++] = 0xeeeeeeeeeeUL;
  DCHECK(index < kDataBufferSize);
  base::OS::PrintError("Data: %p\n", static_cast<void*>(data));
  base::OS::Abort();
}
#endif


static void UpdatePointer(HeapObject** address, HeapObject* object) {
  MapWord map_word = object->map_word();
  // The store buffer can still contain stale pointers in dead large objects.
  // Ignore these pointers here.
  DCHECK(map_word.IsForwardingAddress() ||
         object->GetHeap()->lo_space()->FindPage(
             reinterpret_cast<Address>(address)) != NULL);
  if (map_word.IsForwardingAddress()) {
    // TODO(jochen): Remove again after fixing http://crbug.com/452095
    CHECK((*address)->IsHeapObject() ==
          map_word.ToForwardingAddress()->IsHeapObject());
    // Update the corresponding slot.
    *address = map_word.ToForwardingAddress();
  }
}


static String* UpdateReferenceInExternalStringTableEntry(Heap* heap,
                                                         Object** p) {
  MapWord map_word = HeapObject::cast(*p)->map_word();

  if (map_word.IsForwardingAddress()) {
    return String::cast(map_word.ToForwardingAddress());
  }

  return String::cast(*p);
}


bool MarkCompactCollector::TryPromoteObject(HeapObject* object,
                                            int object_size) {
  DCHECK(object_size <= Page::kMaxRegularHeapObjectSize);

  OldSpace* target_space = heap()->TargetSpace(object);

  DCHECK(target_space == heap()->old_pointer_space() ||
         target_space == heap()->old_data_space());
  HeapObject* target;
  AllocationResult allocation = target_space->AllocateRaw(object_size);
  if (allocation.To(&target)) {
    MigrateObject(target, object, object_size, target_space->identity());
    heap()->IncrementPromotedObjectsSize(object_size);
    return true;
  }

  return false;
}


bool MarkCompactCollector::IsSlotInBlackObject(Page* p, Address slot) {
  // This function does not support large objects right now.
  Space* owner = p->owner();
  if (owner == heap_->lo_space() || owner == NULL) return true;

  uint32_t mark_bit_index = p->AddressToMarkbitIndex(slot);
  unsigned int start_index = mark_bit_index >> Bitmap::kBitsPerCellLog2;
  MarkBit::CellType index_in_cell = 1U
                                    << (mark_bit_index & Bitmap::kBitIndexMask);
  MarkBit::CellType* cells = p->markbits()->cells();
  Address cell_base = p->area_start();
  unsigned int cell_base_start_index = Bitmap::IndexToCell(
      Bitmap::CellAlignIndex(p->AddressToMarkbitIndex(cell_base)));

  // First check if the object is in the current cell.
  MarkBit::CellType slot_mask;
  if ((cells[start_index] == 0) ||
      (base::bits::CountTrailingZeros32(cells[start_index]) >
       base::bits::CountTrailingZeros32(cells[start_index] | index_in_cell))) {
    // If we are already in the first cell, there is no live object.
    if (start_index == cell_base_start_index) return false;

    // If not, find a cell in a preceding cell slot that has a mark bit set.
    do {
      start_index--;
    } while (start_index > cell_base_start_index && cells[start_index] == 0);

    // The slot must be in a dead object if there are no preceding cells that
    // have mark bits set.
    if (cells[start_index] == 0) {
      return false;
    }

    // The object is in a preceding cell. Set the mask to find any object.
    slot_mask = 0xffffffff;
  } else {
    // We are interested in object mark bits right before the slot.
    slot_mask = index_in_cell - 1;
  }

  MarkBit::CellType current_cell = cells[start_index];
  DCHECK(current_cell != 0);

  // Find the last live object in the cell.
  unsigned int leading_zeros =
      base::bits::CountLeadingZeros32(current_cell & slot_mask);
  DCHECK(leading_zeros != 32);
  unsigned int offset = Bitmap::kBitIndexMask - leading_zeros;

  cell_base += (start_index - cell_base_start_index) * 32 * kPointerSize;
  Address address = cell_base + offset * kPointerSize;
  HeapObject* object = HeapObject::FromAddress(address);
  DCHECK(object->address() < reinterpret_cast<Address>(slot));
  if (object->address() <= slot &&
      (object->address() + object->Size()) > slot) {
    // If the slot is within the last found object in the cell, the slot is
    // in a live object.
    return true;
  }
  return false;
}


bool MarkCompactCollector::IsSlotInBlackObjectSlow(Page* p, Address slot) {
  // This function does not support large objects right now.
  Space* owner = p->owner();
  if (owner == heap_->lo_space() || owner == NULL) return true;

  for (MarkBitCellIterator it(p); !it.Done(); it.Advance()) {
    Address cell_base = it.CurrentCellBase();
    MarkBit::CellType* cell = it.CurrentCell();

    MarkBit::CellType current_cell = *cell;
    if (current_cell == 0) continue;

    int offset = 0;
    while (current_cell != 0) {
      int trailing_zeros = base::bits::CountTrailingZeros32(current_cell);
      current_cell >>= trailing_zeros;
      offset += trailing_zeros;
      Address address = cell_base + offset * kPointerSize;

      HeapObject* object = HeapObject::FromAddress(address);
      int size = object->Size();

      if (object->address() > slot) return false;
      if (object->address() <= slot && slot < (object->address() + size)) {
        return true;
      }

      offset++;
      current_cell >>= 1;
    }
  }
  return false;
}


bool MarkCompactCollector::IsSlotInLiveObject(HeapObject** address,
                                              HeapObject* object) {
  // If the target object is not black, the source slot must be part
  // of a non-black (dead) object.
  if (!Marking::IsBlack(Marking::MarkBitFrom(object))) {
    return false;
  }

  // The target object is black but we don't know if the source slot is black.
  // The source object could have died and the slot could be part of a free
  // space. Find out based on mark bits if the slot is part of a live object.
  if (!IsSlotInBlackObject(
          Page::FromAddress(reinterpret_cast<Address>(address)),
          reinterpret_cast<Address>(address))) {
    return false;
  }

  return true;
}


void MarkCompactCollector::VerifyIsSlotInLiveObject(HeapObject** address,
                                                    HeapObject* object) {
  // The target object has to be black.
  CHECK(Marking::IsBlack(Marking::MarkBitFrom(object)));

  // The target object is black but we don't know if the source slot is black.
  // The source object could have died and the slot could be part of a free
  // space. Use the mark bit iterator to find out about liveness of the slot.
  CHECK(IsSlotInBlackObjectSlow(
      Page::FromAddress(reinterpret_cast<Address>(address)),
      reinterpret_cast<Address>(address)));
}


void MarkCompactCollector::EvacuateNewSpace() {
  // There are soft limits in the allocation code, designed trigger a mark
  // sweep collection by failing allocations.  But since we are already in
  // a mark-sweep allocation, there is no sense in trying to trigger one.
  AlwaysAllocateScope scope(isolate());

  NewSpace* new_space = heap()->new_space();

  // Store allocation range before flipping semispaces.
  Address from_bottom = new_space->bottom();
  Address from_top = new_space->top();

  // Flip the semispaces.  After flipping, to space is empty, from space has
  // live objects.
  new_space->Flip();
  new_space->ResetAllocationInfo();

  int survivors_size = 0;

  // First pass: traverse all objects in inactive semispace, remove marks,
  // migrate live objects and write forwarding addresses.  This stage puts
  // new entries in the store buffer and may cause some pages to be marked
  // scan-on-scavenge.
  NewSpacePageIterator it(from_bottom, from_top);
  while (it.has_next()) {
    NewSpacePage* p = it.next();
    survivors_size += DiscoverAndEvacuateBlackObjectsOnPage(new_space, p);
  }

  heap_->IncrementYoungSurvivorsCounter(survivors_size);
  new_space->set_age_mark(new_space->top());
}


void MarkCompactCollector::EvacuateLiveObjectsFromPage(Page* p) {
  AlwaysAllocateScope always_allocate(isolate());
  PagedSpace* space = static_cast<PagedSpace*>(p->owner());
  DCHECK(p->IsEvacuationCandidate() && !p->WasSwept());
  p->SetWasSwept();

  int offsets[16];

  for (MarkBitCellIterator it(p); !it.Done(); it.Advance()) {
    Address cell_base = it.CurrentCellBase();
    MarkBit::CellType* cell = it.CurrentCell();

    if (*cell == 0) continue;

    int live_objects = MarkWordToObjectStarts(*cell, offsets);
    for (int i = 0; i < live_objects; i++) {
      Address object_addr = cell_base + offsets[i] * kPointerSize;
      HeapObject* object = HeapObject::FromAddress(object_addr);
      DCHECK(Marking::IsBlack(Marking::MarkBitFrom(object)));

      int size = object->Size();

      HeapObject* target_object;
      AllocationResult allocation = space->AllocateRaw(size);
      if (!allocation.To(&target_object)) {
        // If allocation failed, use emergency memory and re-try allocation.
        CHECK(space->HasEmergencyMemory());
        space->UseEmergencyMemory();
        allocation = space->AllocateRaw(size);
      }
      if (!allocation.To(&target_object)) {
        // OS refused to give us memory.
        V8::FatalProcessOutOfMemory("Evacuation");
        return;
      }

      MigrateObject(target_object, object, size, space->identity());
      DCHECK(object->map_word().IsForwardingAddress());
    }

    // Clear marking bits for current cell.
    *cell = 0;
  }
  p->ResetLiveBytes();
}


void MarkCompactCollector::EvacuatePages() {
  int npages = evacuation_candidates_.length();
  int abandoned_pages = 0;
  for (int i = 0; i < npages; i++) {
    Page* p = evacuation_candidates_[i];
    DCHECK(p->IsEvacuationCandidate() ||
           p->IsFlagSet(Page::RESCAN_ON_EVACUATION));
    DCHECK(static_cast<int>(p->parallel_sweeping()) ==
           MemoryChunk::SWEEPING_DONE);
    PagedSpace* space = static_cast<PagedSpace*>(p->owner());
    // Allocate emergency memory for the case when compaction fails due to out
    // of memory.
    if (!space->HasEmergencyMemory()) {
      space->CreateEmergencyMemory();
    }
    if (p->IsEvacuationCandidate()) {
      // During compaction we might have to request a new page. Check that we
      // have an emergency page and the space still has room for that.
      if (space->HasEmergencyMemory() || space->CanExpand()) {
        EvacuateLiveObjectsFromPage(p);
        // Unlink the page from the list of pages here. We must not iterate
        // over that page later (e.g. when scan on scavenge pages are
        // processed). The page itself will be freed later and is still
        // reachable from the evacuation candidates list.
        p->Unlink();
      } else {
        // Without room for expansion evacuation is not guaranteed to succeed.
        // Pessimistically abandon unevacuated pages.
        for (int j = i; j < npages; j++) {
          Page* page = evacuation_candidates_[j];
          slots_buffer_allocator_.DeallocateChain(page->slots_buffer_address());
          page->ClearEvacuationCandidate();
          page->SetFlag(Page::RESCAN_ON_EVACUATION);
        }
        abandoned_pages = npages - i;
        break;
      }
    }
  }
  if (npages > 0) {
    // Release emergency memory.
    PagedSpaces spaces(heap());
    for (PagedSpace* space = spaces.next(); space != NULL;
         space = spaces.next()) {
      if (space->HasEmergencyMemory()) {
        space->FreeEmergencyMemory();
      }
    }
    if (FLAG_trace_fragmentation) {
      if (abandoned_pages != 0) {
        PrintF(
            "  Abandon %d out of %d page defragmentations due to lack of "
            "memory\n",
            abandoned_pages, npages);
      } else {
        PrintF("  Defragmented %d pages\n", npages);
      }
    }
  }
}


class EvacuationWeakObjectRetainer : public WeakObjectRetainer {
 public:
  virtual Object* RetainAs(Object* object) {
    if (object->IsHeapObject()) {
      HeapObject* heap_object = HeapObject::cast(object);
      MapWord map_word = heap_object->map_word();
      if (map_word.IsForwardingAddress()) {
        return map_word.ToForwardingAddress();
      }
    }
    return object;
  }
};


static inline void UpdateSlot(Isolate* isolate, ObjectVisitor* v,
                              SlotsBuffer::SlotType slot_type, Address addr) {
  switch (slot_type) {
    case SlotsBuffer::CODE_TARGET_SLOT: {
      RelocInfo rinfo(addr, RelocInfo::CODE_TARGET, 0, NULL);
      rinfo.Visit(isolate, v);
      break;
    }
    case SlotsBuffer::CODE_ENTRY_SLOT: {
      v->VisitCodeEntry(addr);
      break;
    }
    case SlotsBuffer::RELOCATED_CODE_OBJECT: {
      HeapObject* obj = HeapObject::FromAddress(addr);
      Code::cast(obj)->CodeIterateBody(v);
      break;
    }
    case SlotsBuffer::DEBUG_TARGET_SLOT: {
      RelocInfo rinfo(addr, RelocInfo::DEBUG_BREAK_SLOT, 0, NULL);
      if (rinfo.IsPatchedDebugBreakSlotSequence()) rinfo.Visit(isolate, v);
      break;
    }
    case SlotsBuffer::JS_RETURN_SLOT: {
      RelocInfo rinfo(addr, RelocInfo::JS_RETURN, 0, NULL);
      if (rinfo.IsPatchedReturnSequence()) rinfo.Visit(isolate, v);
      break;
    }
    case SlotsBuffer::EMBEDDED_OBJECT_SLOT: {
      RelocInfo rinfo(addr, RelocInfo::EMBEDDED_OBJECT, 0, NULL);
      rinfo.Visit(isolate, v);
      break;
    }
    default:
      UNREACHABLE();
      break;
  }
}


enum SweepingMode { SWEEP_ONLY, SWEEP_AND_VISIT_LIVE_OBJECTS };


enum SkipListRebuildingMode { REBUILD_SKIP_LIST, IGNORE_SKIP_LIST };


enum FreeSpaceTreatmentMode { IGNORE_FREE_SPACE, ZAP_FREE_SPACE };


template <MarkCompactCollector::SweepingParallelism mode>
static intptr_t Free(PagedSpace* space, FreeList* free_list, Address start,
                     int size) {
  if (mode == MarkCompactCollector::SWEEP_ON_MAIN_THREAD) {
    DCHECK(free_list == NULL);
    return space->Free(start, size);
  } else {
    // TODO(hpayer): account for wasted bytes in concurrent sweeping too.
    return size - free_list->Free(start, size);
  }
}


// Sweeps a page. After sweeping the page can be iterated.
// Slots in live objects pointing into evacuation candidates are updated
// if requested.
// Returns the size of the biggest continuous freed memory chunk in bytes.
template <SweepingMode sweeping_mode,
          MarkCompactCollector::SweepingParallelism parallelism,
          SkipListRebuildingMode skip_list_mode,
          FreeSpaceTreatmentMode free_space_mode>
static int Sweep(PagedSpace* space, FreeList* free_list, Page* p,
                 ObjectVisitor* v) {
  DCHECK(!p->IsEvacuationCandidate() && !p->WasSwept());
  DCHECK_EQ(skip_list_mode == REBUILD_SKIP_LIST,
            space->identity() == CODE_SPACE);
  DCHECK((p->skip_list() == NULL) || (skip_list_mode == REBUILD_SKIP_LIST));
  DCHECK(parallelism == MarkCompactCollector::SWEEP_ON_MAIN_THREAD ||
         sweeping_mode == SWEEP_ONLY);

  Address free_start = p->area_start();
  DCHECK(reinterpret_cast<intptr_t>(free_start) % (32 * kPointerSize) == 0);
  int offsets[16];

  SkipList* skip_list = p->skip_list();
  int curr_region = -1;
  if ((skip_list_mode == REBUILD_SKIP_LIST) && skip_list) {
    skip_list->Clear();
  }

  intptr_t freed_bytes = 0;
  intptr_t max_freed_bytes = 0;

  for (MarkBitCellIterator it(p); !it.Done(); it.Advance()) {
    Address cell_base = it.CurrentCellBase();
    MarkBit::CellType* cell = it.CurrentCell();
    int live_objects = MarkWordToObjectStarts(*cell, offsets);
    int live_index = 0;
    for (; live_objects != 0; live_objects--) {
      Address free_end = cell_base + offsets[live_index++] * kPointerSize;
      if (free_end != free_start) {
        int size = static_cast<int>(free_end - free_start);
        if (free_space_mode == ZAP_FREE_SPACE) {
          memset(free_start, 0xcc, size);
        }
        freed_bytes = Free<parallelism>(space, free_list, free_start, size);
        max_freed_bytes = Max(freed_bytes, max_freed_bytes);
      }
      HeapObject* live_object = HeapObject::FromAddress(free_end);
      DCHECK(Marking::IsBlack(Marking::MarkBitFrom(live_object)));
      Map* map = live_object->synchronized_map();
      int size = live_object->SizeFromMap(map);
      if (sweeping_mode == SWEEP_AND_VISIT_LIVE_OBJECTS) {
        live_object->IterateBody(map->instance_type(), size, v);
      }
      if ((skip_list_mode == REBUILD_SKIP_LIST) && skip_list != NULL) {
        int new_region_start = SkipList::RegionNumber(free_end);
        int new_region_end =
            SkipList::RegionNumber(free_end + size - kPointerSize);
        if (new_region_start != curr_region || new_region_end != curr_region) {
          skip_list->AddObject(free_end, size);
          curr_region = new_region_end;
        }
      }
      free_start = free_end + size;
    }
    // Clear marking bits for current cell.
    *cell = 0;
  }
  if (free_start != p->area_end()) {
    int size = static_cast<int>(p->area_end() - free_start);
    if (free_space_mode == ZAP_FREE_SPACE) {
      memset(free_start, 0xcc, size);
    }
    freed_bytes = Free<parallelism>(space, free_list, free_start, size);
    max_freed_bytes = Max(freed_bytes, max_freed_bytes);
  }
  p->ResetLiveBytes();

  if (parallelism == MarkCompactCollector::SWEEP_IN_PARALLEL) {
    // When concurrent sweeping is active, the page will be marked after
    // sweeping by the main thread.
    p->set_parallel_sweeping(MemoryChunk::SWEEPING_FINALIZE);
  } else {
    p->SetWasSwept();
  }
  return FreeList::GuaranteedAllocatable(static_cast<int>(max_freed_bytes));
}


static bool SetMarkBitsUnderInvalidatedCode(Code* code, bool value) {
  Page* p = Page::FromAddress(code->address());

  if (p->IsEvacuationCandidate() || p->IsFlagSet(Page::RESCAN_ON_EVACUATION)) {
    return false;
  }

  Address code_start = code->address();
  Address code_end = code_start + code->Size();

  uint32_t start_index = MemoryChunk::FastAddressToMarkbitIndex(code_start);
  uint32_t end_index =
      MemoryChunk::FastAddressToMarkbitIndex(code_end - kPointerSize);

  Bitmap* b = p->markbits();

  MarkBit start_mark_bit = b->MarkBitFromIndex(start_index);
  MarkBit end_mark_bit = b->MarkBitFromIndex(end_index);

  MarkBit::CellType* start_cell = start_mark_bit.cell();
  MarkBit::CellType* end_cell = end_mark_bit.cell();

  if (value) {
    MarkBit::CellType start_mask = ~(start_mark_bit.mask() - 1);
    MarkBit::CellType end_mask = (end_mark_bit.mask() << 1) - 1;

    if (start_cell == end_cell) {
      *start_cell |= start_mask & end_mask;
    } else {
      *start_cell |= start_mask;
      for (MarkBit::CellType* cell = start_cell + 1; cell < end_cell; cell++) {
        *cell = ~0;
      }
      *end_cell |= end_mask;
    }
  } else {
    for (MarkBit::CellType* cell = start_cell; cell <= end_cell; cell++) {
      *cell = 0;
    }
  }

  return true;
}


static bool IsOnInvalidatedCodeObject(Address addr) {
  // We did not record any slots in large objects thus
  // we can safely go to the page from the slot address.
  Page* p = Page::FromAddress(addr);

  // First check owner's identity because old pointer and old data spaces
  // are swept lazily and might still have non-zero mark-bits on some
  // pages.
  if (p->owner()->identity() != CODE_SPACE) return false;

  // In code space only bits on evacuation candidates (but we don't record
  // any slots on them) and under invalidated code objects are non-zero.
  MarkBit mark_bit =
      p->markbits()->MarkBitFromIndex(Page::FastAddressToMarkbitIndex(addr));

  return mark_bit.Get();
}


void MarkCompactCollector::InvalidateCode(Code* code) {
  if (heap_->incremental_marking()->IsCompacting() &&
      !ShouldSkipEvacuationSlotRecording(code)) {
    DCHECK(compacting_);

    // If the object is white than no slots were recorded on it yet.
    MarkBit mark_bit = Marking::MarkBitFrom(code);
    if (Marking::IsWhite(mark_bit)) return;

    invalidated_code_.Add(code);
  }
}


// Return true if the given code is deoptimized or will be deoptimized.
bool MarkCompactCollector::WillBeDeoptimized(Code* code) {
  return code->is_optimized_code() && code->marked_for_deoptimization();
}


bool MarkCompactCollector::MarkInvalidatedCode() {
  bool code_marked = false;

  int length = invalidated_code_.length();
  for (int i = 0; i < length; i++) {
    Code* code = invalidated_code_[i];

    if (SetMarkBitsUnderInvalidatedCode(code, true)) {
      code_marked = true;
    }
  }

  return code_marked;
}


void MarkCompactCollector::RemoveDeadInvalidatedCode() {
  int length = invalidated_code_.length();
  for (int i = 0; i < length; i++) {
    if (!IsMarked(invalidated_code_[i])) invalidated_code_[i] = NULL;
  }
}


void MarkCompactCollector::ProcessInvalidatedCode(ObjectVisitor* visitor) {
  int length = invalidated_code_.length();
  for (int i = 0; i < length; i++) {
    Code* code = invalidated_code_[i];
    if (code != NULL) {
      code->Iterate(visitor);
      SetMarkBitsUnderInvalidatedCode(code, false);
    }
  }
  invalidated_code_.Rewind(0);
}


void MarkCompactCollector::EvacuateNewSpaceAndCandidates() {
  Heap::RelocationLock relocation_lock(heap());

  bool code_slots_filtering_required;
  {
    GCTracer::Scope gc_scope(heap()->tracer(),
                             GCTracer::Scope::MC_SWEEP_NEWSPACE);
    code_slots_filtering_required = MarkInvalidatedCode();
    EvacuateNewSpace();
  }

  {
    GCTracer::Scope gc_scope(heap()->tracer(),
                             GCTracer::Scope::MC_EVACUATE_PAGES);
    EvacuationScope evacuation_scope(this);
    EvacuatePages();
  }

  // Second pass: find pointers to new space and update them.
  PointersUpdatingVisitor updating_visitor(heap());

  {
    GCTracer::Scope gc_scope(heap()->tracer(),
                             GCTracer::Scope::MC_UPDATE_NEW_TO_NEW_POINTERS);
    // Update pointers in to space.
    SemiSpaceIterator to_it(heap()->new_space()->bottom(),
                            heap()->new_space()->top());
    for (HeapObject* object = to_it.Next(); object != NULL;
         object = to_it.Next()) {
      Map* map = object->map();
      object->IterateBody(map->instance_type(), object->SizeFromMap(map),
                          &updating_visitor);
    }
  }

  {
    GCTracer::Scope gc_scope(heap()->tracer(),
                             GCTracer::Scope::MC_UPDATE_ROOT_TO_NEW_POINTERS);
    // Update roots.
    heap_->IterateRoots(&updating_visitor, VISIT_ALL_IN_SWEEP_NEWSPACE);
  }

  {
    GCTracer::Scope gc_scope(heap()->tracer(),
                             GCTracer::Scope::MC_UPDATE_OLD_TO_NEW_POINTERS);
    StoreBufferRebuildScope scope(heap_, heap_->store_buffer(),
                                  &Heap::ScavengeStoreBufferCallback);
    heap_->store_buffer()->IteratePointersToNewSpace(&UpdatePointer);
  }

  {
    GCTracer::Scope gc_scope(heap()->tracer(),
                             GCTracer::Scope::MC_UPDATE_POINTERS_TO_EVACUATED);
    SlotsBuffer::UpdateSlotsRecordedIn(heap_, migration_slots_buffer_,
                                       code_slots_filtering_required);
    if (FLAG_trace_fragmentation_verbose) {
      PrintF("  migration slots buffer: %d\n",
             SlotsBuffer::SizeOfChain(migration_slots_buffer_));
    }

    if (compacting_ && was_marked_incrementally_) {
      // It's difficult to filter out slots recorded for large objects.
      LargeObjectIterator it(heap_->lo_space());
      for (HeapObject* obj = it.Next(); obj != NULL; obj = it.Next()) {
        // LargeObjectSpace is not swept yet thus we have to skip
        // dead objects explicitly.
        if (!IsMarked(obj)) continue;

        Page* p = Page::FromAddress(obj->address());
        if (p->IsFlagSet(Page::RESCAN_ON_EVACUATION)) {
          obj->Iterate(&updating_visitor);
          p->ClearFlag(Page::RESCAN_ON_EVACUATION);
        }
      }
    }
  }

  int npages = evacuation_candidates_.length();
  {
    GCTracer::Scope gc_scope(
        heap()->tracer(),
        GCTracer::Scope::MC_UPDATE_POINTERS_BETWEEN_EVACUATED);
    for (int i = 0; i < npages; i++) {
      Page* p = evacuation_candidates_[i];
      DCHECK(p->IsEvacuationCandidate() ||
             p->IsFlagSet(Page::RESCAN_ON_EVACUATION));

      if (p->IsEvacuationCandidate()) {
        SlotsBuffer::UpdateSlotsRecordedIn(heap_, p->slots_buffer(),
                                           code_slots_filtering_required);
        if (FLAG_trace_fragmentation_verbose) {
          PrintF("  page %p slots buffer: %d\n", reinterpret_cast<void*>(p),
                 SlotsBuffer::SizeOfChain(p->slots_buffer()));
        }

        // Important: skip list should be cleared only after roots were updated
        // because root iteration traverses the stack and might have to find
        // code objects from non-updated pc pointing into evacuation candidate.
        SkipList* list = p->skip_list();
        if (list != NULL) list->Clear();
      } else {
        if (FLAG_gc_verbose) {
          PrintF("Sweeping 0x%" V8PRIxPTR " during evacuation.\n",
                 reinterpret_cast<intptr_t>(p));
        }
        PagedSpace* space = static_cast<PagedSpace*>(p->owner());
        p->ClearFlag(MemoryChunk::RESCAN_ON_EVACUATION);

        switch (space->identity()) {
          case OLD_DATA_SPACE:
            Sweep<SWEEP_AND_VISIT_LIVE_OBJECTS, SWEEP_ON_MAIN_THREAD,
                  IGNORE_SKIP_LIST, IGNORE_FREE_SPACE>(space, NULL, p,
                                                       &updating_visitor);
            break;
          case OLD_POINTER_SPACE:
            Sweep<SWEEP_AND_VISIT_LIVE_OBJECTS, SWEEP_ON_MAIN_THREAD,
                  IGNORE_SKIP_LIST, IGNORE_FREE_SPACE>(space, NULL, p,
                                                       &updating_visitor);
            break;
          case CODE_SPACE:
            if (FLAG_zap_code_space) {
              Sweep<SWEEP_AND_VISIT_LIVE_OBJECTS, SWEEP_ON_MAIN_THREAD,
                    REBUILD_SKIP_LIST, ZAP_FREE_SPACE>(space, NULL, p,
                                                       &updating_visitor);
            } else {
              Sweep<SWEEP_AND_VISIT_LIVE_OBJECTS, SWEEP_ON_MAIN_THREAD,
                    REBUILD_SKIP_LIST, IGNORE_FREE_SPACE>(space, NULL, p,
                                                          &updating_visitor);
            }
            break;
          default:
            UNREACHABLE();
            break;
        }
      }
    }
  }

  GCTracer::Scope gc_scope(heap()->tracer(),
                           GCTracer::Scope::MC_UPDATE_MISC_POINTERS);

  // Update pointers from cells.
  HeapObjectIterator cell_iterator(heap_->cell_space());
  for (HeapObject* cell = cell_iterator.Next(); cell != NULL;
       cell = cell_iterator.Next()) {
    if (cell->IsCell()) {
      Cell::BodyDescriptor::IterateBody(cell, &updating_visitor);
    }
  }

  heap_->string_table()->Iterate(&updating_visitor);

  // Update pointers from external string table.
  heap_->UpdateReferencesInExternalStringTable(
      &UpdateReferenceInExternalStringTableEntry);

  EvacuationWeakObjectRetainer evacuation_object_retainer;
  heap()->ProcessAllWeakReferences(&evacuation_object_retainer);

  // Collects callback info for handles that are pending (about to be
  // collected) and either phantom or internal-fields.  Releases the global
  // handles.  See also PostGarbageCollectionProcessing.
  isolate()->global_handles()->CollectAllPhantomCallbackData();

  // Visit invalidated code (we ignored all slots on it) and clear mark-bits
  // under it.
  ProcessInvalidatedCode(&updating_visitor);

  heap_->isolate()->inner_pointer_to_code_cache()->Flush();

  slots_buffer_allocator_.DeallocateChain(&migration_slots_buffer_);
  DCHECK(migration_slots_buffer_ == NULL);

  // The hashing of weak_object_to_code_table is no longer valid.
  heap()->weak_object_to_code_table()->Rehash(
      heap()->isolate()->factory()->undefined_value());
}


void MarkCompactCollector::MoveEvacuationCandidatesToEndOfPagesList() {
  int npages = evacuation_candidates_.length();
  for (int i = 0; i < npages; i++) {
    Page* p = evacuation_candidates_[i];
    if (!p->IsEvacuationCandidate()) continue;
    p->Unlink();
    PagedSpace* space = static_cast<PagedSpace*>(p->owner());
    p->InsertAfter(space->LastPage());
  }
}


void MarkCompactCollector::ReleaseEvacuationCandidates() {
  int npages = evacuation_candidates_.length();
  for (int i = 0; i < npages; i++) {
    Page* p = evacuation_candidates_[i];
    if (!p->IsEvacuationCandidate()) continue;
    PagedSpace* space = static_cast<PagedSpace*>(p->owner());
    space->Free(p->area_start(), p->area_size());
    p->set_scan_on_scavenge(false);
    slots_buffer_allocator_.DeallocateChain(p->slots_buffer_address());
    p->ResetLiveBytes();
    space->ReleasePage(p);
  }
  evacuation_candidates_.Rewind(0);
  compacting_ = false;
  heap()->FreeQueuedChunks();
}


static const int kStartTableEntriesPerLine = 5;
static const int kStartTableLines = 171;
static const int kStartTableInvalidLine = 127;
static const int kStartTableUnusedEntry = 126;

#define _ kStartTableUnusedEntry
#define X kStartTableInvalidLine
// Mark-bit to object start offset table.
//
// The line is indexed by the mark bits in a byte.  The first number on
// the line describes the number of live object starts for the line and the
// other numbers on the line describe the offsets (in words) of the object
// starts.
//
// Since objects are at least 2 words large we don't have entries for two
// consecutive 1 bits.  All entries after 170 have at least 2 consecutive bits.
char kStartTable[kStartTableLines * kStartTableEntriesPerLine] = {
    0, _, _,
    _, _,  // 0
    1, 0, _,
    _, _,  // 1
    1, 1, _,
    _, _,  // 2
    X, _, _,
    _, _,  // 3
    1, 2, _,
    _, _,  // 4
    2, 0, 2,
    _, _,  // 5
    X, _, _,
    _, _,  // 6
    X, _, _,
    _, _,  // 7
    1, 3, _,
    _, _,  // 8
    2, 0, 3,
    _, _,  // 9
    2, 1, 3,
    _, _,  // 10
    X, _, _,
    _, _,  // 11
    X, _, _,
    _, _,  // 12
    X, _, _,
    _, _,  // 13
    X, _, _,
    _, _,  // 14
    X, _, _,
    _, _,  // 15
    1, 4, _,
    _, _,  // 16
    2, 0, 4,
    _, _,  // 17
    2, 1, 4,
    _, _,  // 18
    X, _, _,
    _, _,  // 19
    2, 2, 4,
    _, _,  // 20
    3, 0, 2,
    4, _,  // 21
    X, _, _,
    _, _,  // 22
    X, _, _,
    _, _,  // 23
    X, _, _,
    _, _,  // 24
    X, _, _,
    _, _,  // 25
    X, _, _,
    _, _,  // 26
    X, _, _,
    _, _,  // 27
    X, _, _,
    _, _,  // 28
    X, _, _,
    _, _,  // 29
    X, _, _,
    _, _,  // 30
    X, _, _,
    _, _,  // 31
    1, 5, _,
    _, _,  // 32
    2, 0, 5,
    _, _,  // 33
    2, 1, 5,
    _, _,  // 34
    X, _, _,
    _, _,  // 35
    2, 2, 5,
    _, _,  // 36
    3, 0, 2,
    5, _,  // 37
    X, _, _,
    _, _,  // 38
    X, _, _,
    _, _,  // 39
    2, 3, 5,
    _, _,  // 40
    3, 0, 3,
    5, _,  // 41
    3, 1, 3,
    5, _,  // 42
    X, _, _,
    _, _,  // 43
    X, _, _,
    _, _,  // 44
    X, _, _,
    _, _,  // 45
    X, _, _,
    _, _,  // 46
    X, _, _,
    _, _,  // 47
    X, _, _,
    _, _,  // 48
    X, _, _,
    _, _,  // 49
    X, _, _,
    _, _,  // 50
    X, _, _,
    _, _,  // 51
    X, _, _,
    _, _,  // 52
    X, _, _,
    _, _,  // 53
    X, _, _,
    _, _,  // 54
    X, _, _,
    _, _,  // 55
    X, _, _,
    _, _,  // 56
    X, _, _,
    _, _,  // 57
    X, _, _,
    _, _,  // 58
    X, _, _,
    _, _,  // 59
    X, _, _,
    _, _,  // 60
    X, _, _,
    _, _,  // 61
    X, _, _,
    _, _,  // 62
    X, _, _,
    _, _,  // 63
    1, 6, _,
    _, _,  // 64
    2, 0, 6,
    _, _,  // 65
    2, 1, 6,
    _, _,  // 66
    X, _, _,
    _, _,  // 67
    2, 2, 6,
    _, _,  // 68
    3, 0, 2,
    6, _,  // 69
    X, _, _,
    _, _,  // 70
    X, _, _,
    _, _,  // 71
    2, 3, 6,
    _, _,  // 72
    3, 0, 3,
    6, _,  // 73
    3, 1, 3,
    6, _,  // 74
    X, _, _,
    _, _,  // 75
    X, _, _,
    _, _,  // 76
    X, _, _,
    _, _,  // 77
    X, _, _,
    _, _,  // 78
    X, _, _,
    _, _,  // 79
    2, 4, 6,
    _, _,  // 80
    3, 0, 4,
    6, _,  // 81
    3, 1, 4,
    6, _,  // 82
    X, _, _,
    _, _,  // 83
    3, 2, 4,
    6, _,  // 84
    4, 0, 2,
    4, 6,  // 85
    X, _, _,
    _, _,  // 86
    X, _, _,
    _, _,  // 87
    X, _, _,
    _, _,  // 88
    X, _, _,
    _, _,  // 89
    X, _, _,
    _, _,  // 90
    X, _, _,
    _, _,  // 91
    X, _, _,
    _, _,  // 92
    X, _, _,
    _, _,  // 93
    X, _, _,
    _, _,  // 94
    X, _, _,
    _, _,  // 95
    X, _, _,
    _, _,  // 96
    X, _, _,
    _, _,  // 97
    X, _, _,
    _, _,  // 98
    X, _, _,
    _, _,  // 99
    X, _, _,
    _, _,  // 100
    X, _, _,
    _, _,  // 101
    X, _, _,
    _, _,  // 102
    X, _, _,
    _, _,  // 103
    X, _, _,
    _, _,  // 104
    X, _, _,
    _, _,  // 105
    X, _, _,
    _, _,  // 106
    X, _, _,
    _, _,  // 107
    X, _, _,
    _, _,  // 108
    X, _, _,
    _, _,  // 109
    X, _, _,
    _, _,  // 110
    X, _, _,
    _, _,  // 111
    X, _, _,
    _, _,  // 112
    X, _, _,
    _, _,  // 113
    X, _, _,
    _, _,  // 114
    X, _, _,
    _, _,  // 115
    X, _, _,
    _, _,  // 116
    X, _, _,
    _, _,  // 117
    X, _, _,
    _, _,  // 118
    X, _, _,
    _, _,  // 119
    X, _, _,
    _, _,  // 120
    X, _, _,
    _, _,  // 121
    X, _, _,
    _, _,  // 122
    X, _, _,
    _, _,  // 123
    X, _, _,
    _, _,  // 124
    X, _, _,
    _, _,  // 125
    X, _, _,
    _, _,  // 126
    X, _, _,
    _, _,  // 127
    1, 7, _,
    _, _,  // 128
    2, 0, 7,
    _, _,  // 129
    2, 1, 7,
    _, _,  // 130
    X, _, _,
    _, _,  // 131
    2, 2, 7,
    _, _,  // 132
    3, 0, 2,
    7, _,  // 133
    X, _, _,
    _, _,  // 134
    X, _, _,
    _, _,  // 135
    2, 3, 7,
    _, _,  // 136
    3, 0, 3,
    7, _,  // 137
    3, 1, 3,
    7, _,  // 138
    X, _, _,
    _, _,  // 139
    X, _, _,
    _, _,  // 140
    X, _, _,
    _, _,  // 141
    X, _, _,
    _, _,  // 142
    X, _, _,
    _, _,  // 143
    2, 4, 7,
    _, _,  // 144
    3, 0, 4,
    7, _,  // 145
    3, 1, 4,
    7, _,  // 146
    X, _, _,
    _, _,  // 147
    3, 2, 4,
    7, _,  // 148
    4, 0, 2,
    4, 7,  // 149
    X, _, _,
    _, _,  // 150
    X, _, _,
    _, _,  // 151
    X, _, _,
    _, _,  // 152
    X, _, _,
    _, _,  // 153
    X, _, _,
    _, _,  // 154
    X, _, _,
    _, _,  // 155
    X, _, _,
    _, _,  // 156
    X, _, _,
    _, _,  // 157
    X, _, _,
    _, _,  // 158
    X, _, _,
    _, _,  // 159
    2, 5, 7,
    _, _,  // 160
    3, 0, 5,
    7, _,  // 161
    3, 1, 5,
    7, _,  // 162
    X, _, _,
    _, _,  // 163
    3, 2, 5,
    7, _,  // 164
    4, 0, 2,
    5, 7,  // 165
    X, _, _,
    _, _,  // 166
    X, _, _,
    _, _,  // 167
    3, 3, 5,
    7, _,  // 168
    4, 0, 3,
    5, 7,  // 169
    4, 1, 3,
    5, 7  // 170
};
#undef _
#undef X


// Takes a word of mark bits.  Returns the number of objects that start in the
// range.  Puts the offsets of the words in the supplied array.
static inline int MarkWordToObjectStarts(uint32_t mark_bits, int* starts) {
  int objects = 0;
  int offset = 0;

  // No consecutive 1 bits.
  DCHECK((mark_bits & 0x180) != 0x180);
  DCHECK((mark_bits & 0x18000) != 0x18000);
  DCHECK((mark_bits & 0x1800000) != 0x1800000);

  while (mark_bits != 0) {
    int byte = (mark_bits & 0xff);
    mark_bits >>= 8;
    if (byte != 0) {
      DCHECK(byte < kStartTableLines);  // No consecutive 1 bits.
      char* table = kStartTable + byte * kStartTableEntriesPerLine;
      int objects_in_these_8_words = table[0];
      DCHECK(objects_in_these_8_words != kStartTableInvalidLine);
      DCHECK(objects_in_these_8_words < kStartTableEntriesPerLine);
      for (int i = 0; i < objects_in_these_8_words; i++) {
        starts[objects++] = offset + table[1 + i];
      }
    }
    offset += 8;
  }
  return objects;
}


int MarkCompactCollector::SweepInParallel(PagedSpace* space,
                                          int required_freed_bytes) {
  int max_freed = 0;
  int max_freed_overall = 0;
  PageIterator it(space);
  while (it.has_next()) {
    Page* p = it.next();
    max_freed = SweepInParallel(p, space);
    DCHECK(max_freed >= 0);
    if (required_freed_bytes > 0 && max_freed >= required_freed_bytes) {
      return max_freed;
    }
    max_freed_overall = Max(max_freed, max_freed_overall);
    if (p == space->end_of_unswept_pages()) break;
  }
  return max_freed_overall;
}


int MarkCompactCollector::SweepInParallel(Page* page, PagedSpace* space) {
  int max_freed = 0;
  if (page->TryParallelSweeping()) {
    FreeList* free_list = space == heap()->old_pointer_space()
                              ? free_list_old_pointer_space_.get()
                              : free_list_old_data_space_.get();
    FreeList private_free_list(space);
    max_freed = Sweep<SWEEP_ONLY, SWEEP_IN_PARALLEL, IGNORE_SKIP_LIST,
                      IGNORE_FREE_SPACE>(space, &private_free_list, page, NULL);
    free_list->Concatenate(&private_free_list);
  }
  return max_freed;
}


void MarkCompactCollector::SweepSpace(PagedSpace* space, SweeperType sweeper) {
  space->ClearStats();

  // We defensively initialize end_of_unswept_pages_ here with the first page
  // of the pages list.
  space->set_end_of_unswept_pages(space->FirstPage());

  PageIterator it(space);

  int pages_swept = 0;
  bool unused_page_present = false;
  bool parallel_sweeping_active = false;

  while (it.has_next()) {
    Page* p = it.next();
    DCHECK(p->parallel_sweeping() == MemoryChunk::SWEEPING_DONE);

    // Clear sweeping flags indicating that marking bits are still intact.
    p->ClearWasSwept();

    if (p->IsFlagSet(Page::RESCAN_ON_EVACUATION) ||
        p->IsEvacuationCandidate()) {
      // Will be processed in EvacuateNewSpaceAndCandidates.
      DCHECK(evacuation_candidates_.length() > 0);
      continue;
    }

    // One unused page is kept, all further are released before sweeping them.
    if (p->LiveBytes() == 0) {
      if (unused_page_present) {
        if (FLAG_gc_verbose) {
          PrintF("Sweeping 0x%" V8PRIxPTR " released page.\n",
                 reinterpret_cast<intptr_t>(p));
        }
        // Adjust unswept free bytes because releasing a page expects said
        // counter to be accurate for unswept pages.
        space->IncreaseUnsweptFreeBytes(p);
        space->ReleasePage(p);
        continue;
      }
      unused_page_present = true;
    }

    switch (sweeper) {
      case CONCURRENT_SWEEPING:
        if (!parallel_sweeping_active) {
          if (FLAG_gc_verbose) {
            PrintF("Sweeping 0x%" V8PRIxPTR ".\n",
                   reinterpret_cast<intptr_t>(p));
          }
          Sweep<SWEEP_ONLY, SWEEP_ON_MAIN_THREAD, IGNORE_SKIP_LIST,
                IGNORE_FREE_SPACE>(space, NULL, p, NULL);
          pages_swept++;
          parallel_sweeping_active = true;
        } else {
          if (FLAG_gc_verbose) {
            PrintF("Sweeping 0x%" V8PRIxPTR " in parallel.\n",
                   reinterpret_cast<intptr_t>(p));
          }
          p->set_parallel_sweeping(MemoryChunk::SWEEPING_PENDING);
          space->IncreaseUnsweptFreeBytes(p);
        }
        space->set_end_of_unswept_pages(p);
        break;
      case SEQUENTIAL_SWEEPING: {
        if (FLAG_gc_verbose) {
          PrintF("Sweeping 0x%" V8PRIxPTR ".\n", reinterpret_cast<intptr_t>(p));
        }
        if (space->identity() == CODE_SPACE && FLAG_zap_code_space) {
          Sweep<SWEEP_ONLY, SWEEP_ON_MAIN_THREAD, REBUILD_SKIP_LIST,
                ZAP_FREE_SPACE>(space, NULL, p, NULL);
        } else if (space->identity() == CODE_SPACE) {
          Sweep<SWEEP_ONLY, SWEEP_ON_MAIN_THREAD, REBUILD_SKIP_LIST,
                IGNORE_FREE_SPACE>(space, NULL, p, NULL);
        } else {
          Sweep<SWEEP_ONLY, SWEEP_ON_MAIN_THREAD, IGNORE_SKIP_LIST,
                IGNORE_FREE_SPACE>(space, NULL, p, NULL);
        }
        pages_swept++;
        break;
      }
      default: { UNREACHABLE(); }
    }
  }

  if (FLAG_gc_verbose) {
    PrintF("SweepSpace: %s (%d pages swept)\n",
           AllocationSpaceName(space->identity()), pages_swept);
  }

  // Give pages that are queued to be freed back to the OS.
  heap()->FreeQueuedChunks();
}


void MarkCompactCollector::SweepSpaces() {
  GCTracer::Scope gc_scope(heap()->tracer(), GCTracer::Scope::MC_SWEEP);
  double start_time = 0.0;
  if (FLAG_print_cumulative_gc_stat) {
    start_time = base::OS::TimeCurrentMillis();
  }

#ifdef DEBUG
  state_ = SWEEP_SPACES;
#endif
  MoveEvacuationCandidatesToEndOfPagesList();

  // Noncompacting collections simply sweep the spaces to clear the mark
  // bits and free the nonlive blocks (for old and map spaces).  We sweep
  // the map space last because freeing non-live maps overwrites them and
  // the other spaces rely on possibly non-live maps to get the sizes for
  // non-live objects.
  {
    GCTracer::Scope sweep_scope(heap()->tracer(),
                                GCTracer::Scope::MC_SWEEP_OLDSPACE);
    {
      SweepSpace(heap()->old_pointer_space(), CONCURRENT_SWEEPING);
      SweepSpace(heap()->old_data_space(), CONCURRENT_SWEEPING);
    }
    sweeping_in_progress_ = true;
    if (heap()->concurrent_sweeping_enabled()) {
      StartSweeperThreads();
    }
  }
  RemoveDeadInvalidatedCode();

  {
    GCTracer::Scope sweep_scope(heap()->tracer(),
                                GCTracer::Scope::MC_SWEEP_CODE);
    SweepSpace(heap()->code_space(), SEQUENTIAL_SWEEPING);
  }

  {
    GCTracer::Scope sweep_scope(heap()->tracer(),
                                GCTracer::Scope::MC_SWEEP_CELL);
    SweepSpace(heap()->cell_space(), SEQUENTIAL_SWEEPING);
  }

  EvacuateNewSpaceAndCandidates();

  // ClearNonLiveReferences depends on precise sweeping of map space to
  // detect whether unmarked map became dead in this collection or in one
  // of the previous ones.
  {
    GCTracer::Scope sweep_scope(heap()->tracer(),
                                GCTracer::Scope::MC_SWEEP_MAP);
    SweepSpace(heap()->map_space(), SEQUENTIAL_SWEEPING);
  }

  // Deallocate unmarked objects and clear marked bits for marked objects.
  heap_->lo_space()->FreeUnmarkedObjects();

  // Deallocate evacuated candidate pages.
  ReleaseEvacuationCandidates();
  CodeRange* code_range = heap()->isolate()->code_range();
  if (code_range != NULL && code_range->valid()) {
    code_range->ReserveEmergencyBlock();
  }

  if (FLAG_print_cumulative_gc_stat) {
    heap_->tracer()->AddSweepingTime(base::OS::TimeCurrentMillis() -
                                     start_time);
  }
}


void MarkCompactCollector::ParallelSweepSpaceComplete(PagedSpace* space) {
  PageIterator it(space);
  while (it.has_next()) {
    Page* p = it.next();
    if (p->parallel_sweeping() == MemoryChunk::SWEEPING_FINALIZE) {
      p->set_parallel_sweeping(MemoryChunk::SWEEPING_DONE);
      p->SetWasSwept();
    }
    DCHECK(p->parallel_sweeping() == MemoryChunk::SWEEPING_DONE);
  }
}


void MarkCompactCollector::ParallelSweepSpacesComplete() {
  ParallelSweepSpaceComplete(heap()->old_pointer_space());
  ParallelSweepSpaceComplete(heap()->old_data_space());
}


void MarkCompactCollector::EnableCodeFlushing(bool enable) {
  if (isolate()->debug()->is_loaded() ||
      isolate()->debug()->has_break_points()) {
    enable = false;
  }

  if (enable) {
    if (code_flusher_ != NULL) return;
    code_flusher_ = new CodeFlusher(isolate());
  } else {
    if (code_flusher_ == NULL) return;
    code_flusher_->EvictAllCandidates();
    delete code_flusher_;
    code_flusher_ = NULL;
  }

  if (FLAG_trace_code_flushing) {
    PrintF("[code-flushing is now %s]\n", enable ? "on" : "off");
  }
}


// TODO(1466) ReportDeleteIfNeeded is not called currently.
// Our profiling tools do not expect intersections between
// code objects. We should either reenable it or change our tools.
void MarkCompactCollector::ReportDeleteIfNeeded(HeapObject* obj,
                                                Isolate* isolate) {
  if (obj->IsCode()) {
    PROFILE(isolate, CodeDeleteEvent(obj->address()));
  }
}


Isolate* MarkCompactCollector::isolate() const { return heap_->isolate(); }


void MarkCompactCollector::Initialize() {
  MarkCompactMarkingVisitor::Initialize();
  IncrementalMarking::Initialize();
}


bool SlotsBuffer::IsTypedSlot(ObjectSlot slot) {
  return reinterpret_cast<uintptr_t>(slot) < NUMBER_OF_SLOT_TYPES;
}


bool SlotsBuffer::AddTo(SlotsBufferAllocator* allocator,
                        SlotsBuffer** buffer_address, SlotType type,
                        Address addr, AdditionMode mode) {
  SlotsBuffer* buffer = *buffer_address;
  if (buffer == NULL || !buffer->HasSpaceForTypedSlot()) {
    if (mode == FAIL_ON_OVERFLOW && ChainLengthThresholdReached(buffer)) {
      allocator->DeallocateChain(buffer_address);
      return false;
    }
    buffer = allocator->AllocateBuffer(buffer);
    *buffer_address = buffer;
  }
  DCHECK(buffer->HasSpaceForTypedSlot());
  buffer->Add(reinterpret_cast<ObjectSlot>(type));
  buffer->Add(reinterpret_cast<ObjectSlot>(addr));
  return true;
}


static inline SlotsBuffer::SlotType SlotTypeForRMode(RelocInfo::Mode rmode) {
  if (RelocInfo::IsCodeTarget(rmode)) {
    return SlotsBuffer::CODE_TARGET_SLOT;
  } else if (RelocInfo::IsEmbeddedObject(rmode)) {
    return SlotsBuffer::EMBEDDED_OBJECT_SLOT;
  } else if (RelocInfo::IsDebugBreakSlot(rmode)) {
    return SlotsBuffer::DEBUG_TARGET_SLOT;
  } else if (RelocInfo::IsJSReturn(rmode)) {
    return SlotsBuffer::JS_RETURN_SLOT;
  }
  UNREACHABLE();
  return SlotsBuffer::NUMBER_OF_SLOT_TYPES;
}


void MarkCompactCollector::RecordRelocSlot(RelocInfo* rinfo, Object* target) {
  Page* target_page = Page::FromAddress(reinterpret_cast<Address>(target));
  RelocInfo::Mode rmode = rinfo->rmode();
  if (target_page->IsEvacuationCandidate() &&
      (rinfo->host() == NULL ||
       !ShouldSkipEvacuationSlotRecording(rinfo->host()))) {
    bool success;
    if (RelocInfo::IsEmbeddedObject(rmode) && rinfo->IsInConstantPool()) {
      // This doesn't need to be typed since it is just a normal heap pointer.
      Object** target_pointer =
          reinterpret_cast<Object**>(rinfo->constant_pool_entry_address());
      success = SlotsBuffer::AddTo(
          &slots_buffer_allocator_, target_page->slots_buffer_address(),
          target_pointer, SlotsBuffer::FAIL_ON_OVERFLOW);
    } else if (RelocInfo::IsCodeTarget(rmode) && rinfo->IsInConstantPool()) {
      success = SlotsBuffer::AddTo(
          &slots_buffer_allocator_, target_page->slots_buffer_address(),
          SlotsBuffer::CODE_ENTRY_SLOT, rinfo->constant_pool_entry_address(),
          SlotsBuffer::FAIL_ON_OVERFLOW);
    } else {
      success = SlotsBuffer::AddTo(
          &slots_buffer_allocator_, target_page->slots_buffer_address(),
          SlotTypeForRMode(rmode), rinfo->pc(), SlotsBuffer::FAIL_ON_OVERFLOW);
    }
    if (!success) {
      EvictEvacuationCandidate(target_page);
    }
  }
}


void MarkCompactCollector::RecordCodeEntrySlot(Address slot, Code* target) {
  Page* target_page = Page::FromAddress(reinterpret_cast<Address>(target));
  if (target_page->IsEvacuationCandidate() &&
      !ShouldSkipEvacuationSlotRecording(reinterpret_cast<Object**>(slot))) {
    if (!SlotsBuffer::AddTo(&slots_buffer_allocator_,
                            target_page->slots_buffer_address(),
                            SlotsBuffer::CODE_ENTRY_SLOT, slot,
                            SlotsBuffer::FAIL_ON_OVERFLOW)) {
      EvictEvacuationCandidate(target_page);
    }
  }
}


void MarkCompactCollector::RecordCodeTargetPatch(Address pc, Code* target) {
  DCHECK(heap()->gc_state() == Heap::MARK_COMPACT);
  if (is_compacting()) {
    Code* host =
        isolate()->inner_pointer_to_code_cache()->GcSafeFindCodeForInnerPointer(
            pc);
    MarkBit mark_bit = Marking::MarkBitFrom(host);
    if (Marking::IsBlack(mark_bit)) {
      RelocInfo rinfo(pc, RelocInfo::CODE_TARGET, 0, host);
      RecordRelocSlot(&rinfo, target);
    }
  }
}


static inline SlotsBuffer::SlotType DecodeSlotType(
    SlotsBuffer::ObjectSlot slot) {
  return static_cast<SlotsBuffer::SlotType>(reinterpret_cast<intptr_t>(slot));
}


void SlotsBuffer::UpdateSlots(Heap* heap) {
  PointersUpdatingVisitor v(heap);

  for (int slot_idx = 0; slot_idx < idx_; ++slot_idx) {
    ObjectSlot slot = slots_[slot_idx];
    if (!IsTypedSlot(slot)) {
      PointersUpdatingVisitor::UpdateSlot(heap, slot);
    } else {
      ++slot_idx;
      DCHECK(slot_idx < idx_);
      UpdateSlot(heap->isolate(), &v, DecodeSlotType(slot),
                 reinterpret_cast<Address>(slots_[slot_idx]));
    }
  }
}


void SlotsBuffer::UpdateSlotsWithFilter(Heap* heap) {
  PointersUpdatingVisitor v(heap);

  for (int slot_idx = 0; slot_idx < idx_; ++slot_idx) {
    ObjectSlot slot = slots_[slot_idx];
    if (!IsTypedSlot(slot)) {
      if (!IsOnInvalidatedCodeObject(reinterpret_cast<Address>(slot))) {
        PointersUpdatingVisitor::UpdateSlot(heap, slot);
      }
    } else {
      ++slot_idx;
      DCHECK(slot_idx < idx_);
      Address pc = reinterpret_cast<Address>(slots_[slot_idx]);
      if (!IsOnInvalidatedCodeObject(pc)) {
        UpdateSlot(heap->isolate(), &v, DecodeSlotType(slot),
                   reinterpret_cast<Address>(slots_[slot_idx]));
      }
    }
  }
}


SlotsBuffer* SlotsBufferAllocator::AllocateBuffer(SlotsBuffer* next_buffer) {
  return new SlotsBuffer(next_buffer);
}


void SlotsBufferAllocator::DeallocateBuffer(SlotsBuffer* buffer) {
  delete buffer;
}


void SlotsBufferAllocator::DeallocateChain(SlotsBuffer** buffer_address) {
  SlotsBuffer* buffer = *buffer_address;
  while (buffer != NULL) {
    SlotsBuffer* next_buffer = buffer->next();
    DeallocateBuffer(buffer);
    buffer = next_buffer;
  }
  *buffer_address = NULL;
}
}
}  // namespace v8::internal
