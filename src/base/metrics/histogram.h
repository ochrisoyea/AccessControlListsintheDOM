// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Histogram is an object that aggregates statistics, and can summarize them in
// various forms, including ASCII graphical, HTML, and numerically (as a
// vector of numbers corresponding to each of the aggregating buckets).

// It supports calls to accumulate either time intervals (which are processed
// as integral number of milliseconds), or arbitrary integral units.

// For Histogram (exponential histogram), LinearHistogram and CustomHistogram,
// the minimum for a declared range is 1 (instead of 0), while the maximum is
// (HistogramBase::kSampleType_MAX - 1). However, there will always be underflow
// and overflow buckets added automatically, so a 0 bucket will always exist
// even when a minimum value of 1 is specified.

// Each use of a histogram with the same name will reference the same underlying
// data, so it is safe to record to the same histogram from multiple locations
// in the code. It is a runtime error if all uses of the same histogram do not
// agree exactly in type, bucket size and range.

// For Histogram and LinearHistogram, the maximum for a declared range should
// always be larger (not equal) than minimal range. Zero and
// HistogramBase::kSampleType_MAX are implicitly added as first and last ranges,
// so the smallest legal bucket_count is 3. However CustomHistogram can have
// bucket count as 2 (when you give a custom ranges vector containing only 1
// range).
// For these 3 kinds of histograms, the max bucket count is always
// (Histogram::kBucketCount_MAX - 1).

// The buckets layout of class Histogram is exponential. For example, buckets
// might contain (sequentially) the count of values in the following intervals:
// [0,1), [1,2), [2,4), [4,8), [8,16), [16,32), [32,64), [64,infinity)
// That bucket allocation would actually result from construction of a histogram
// for values between 1 and 64, with 8 buckets, such as:
// Histogram count("some name", 1, 64, 8);
// Note that the underflow bucket [0,1) and the overflow bucket [64,infinity)
// are also counted by the constructor in the user supplied "bucket_count"
// argument.
// The above example has an exponential ratio of 2 (doubling the bucket width
// in each consecutive bucket).  The Histogram class automatically calculates
// the smallest ratio that it can use to construct the number of buckets
// selected in the constructor.  An another example, if you had 50 buckets,
// and millisecond time values from 1 to 10000, then the ratio between
// consecutive bucket widths will be approximately somewhere around the 50th
// root of 10000.  This approach provides very fine grain (narrow) buckets
// at the low end of the histogram scale, but allows the histogram to cover a
// gigantic range with the addition of very few buckets.

// Usually we use macros to define and use a histogram, which are defined in
// base/metrics/histogram_macros.h. Note: Callers should include that header
// directly if they only access the histogram APIs through macros.
//
// Macros use a pattern involving a function static variable, that is a pointer
// to a histogram.  This static is explicitly initialized on any thread
// that detects a uninitialized (NULL) pointer.  The potentially racy
// initialization is not a problem as it is always set to point to the same
// value (i.e., the FactoryGet always returns the same value).  FactoryGet
// is also completely thread safe, which results in a completely thread safe,
// and relatively fast, set of counters.  To avoid races at shutdown, the static
// pointer is NOT deleted, and we leak the histograms at process termination.

#ifndef BASE_METRICS_HISTOGRAM_H_
#define BASE_METRICS_HISTOGRAM_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/base_export.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/gtest_prod_util.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/metrics/bucket_ranges.h"
#include "base/metrics/histogram_base.h"
#include "base/metrics/histogram_samples.h"
#include "base/strings/string_piece.h"
#include "base/time/time.h"

namespace base {

class BooleanHistogram;
class CustomHistogram;
class DelayedPersistentAllocation;
class Histogram;
class LinearHistogram;
class Pickle;
class PickleIterator;
class SampleVector;
class SampleVectorBase;

class BASE_EXPORT Histogram : public HistogramBase {
 public:
  // Initialize maximum number of buckets in histograms as 16,384.
  static const uint32_t kBucketCount_MAX;

  typedef std::vector<Count> Counts;

  ~Histogram() override;

  //----------------------------------------------------------------------------
  // For a valid histogram, input should follow these restrictions:
  // minimum > 0 (if a minimum below 1 is specified, it will implicitly be
  //              normalized up to 1)
  // maximum > minimum
  // buckets > 2 [minimum buckets needed: underflow, overflow and the range]
  // Additionally,
  // buckets <= (maximum - minimum + 2) - this is to ensure that we don't have
  // more buckets than the range of numbers; having more buckets than 1 per
  // value in the range would be nonsensical.
  static HistogramBase* FactoryGet(const std::string& name,
                                   Sample minimum,
                                   Sample maximum,
                                   uint32_t bucket_count,
                                   int32_t flags);
  static HistogramBase* FactoryTimeGet(const std::string& name,
                                       base::TimeDelta minimum,
                                       base::TimeDelta maximum,
                                       uint32_t bucket_count,
                                       int32_t flags);

  // Overloads of the above two functions that take a const char* |name| param,
  // to avoid code bloat from the std::string constructor being inlined into
  // call sites.
  static HistogramBase* FactoryGet(const char* name,
                                   Sample minimum,
                                   Sample maximum,
                                   uint32_t bucket_count,
                                   int32_t flags);
  static HistogramBase* FactoryTimeGet(const char* name,
                                       base::TimeDelta minimum,
                                       base::TimeDelta maximum,
                                       uint32_t bucket_count,
                                       int32_t flags);

  // Create a histogram using data in persistent storage.
  static std::unique_ptr<HistogramBase> PersistentCreate(
      const char* name,
      Sample minimum,
      Sample maximum,
      const BucketRanges* ranges,
      const DelayedPersistentAllocation& counts,
      const DelayedPersistentAllocation& logged_counts,
      HistogramSamples::Metadata* meta,
      HistogramSamples::Metadata* logged_meta);

  static void InitializeBucketRanges(Sample minimum,
                                     Sample maximum,
                                     BucketRanges* ranges);

  // This constant if for FindCorruption. Since snapshots of histograms are
  // taken asynchronously relative to sampling, and our counting code currently
  // does not prevent race conditions, it is pretty likely that we'll catch a
  // redundant count that doesn't match the sample count.  We allow for a
  // certain amount of slop before flagging this as an inconsistency. Even with
  // an inconsistency, we'll snapshot it again (for UMA in about a half hour),
  // so we'll eventually get the data, if it was not the result of a corruption.
  static const int kCommonRaceBasedCountMismatch;

  // Check to see if bucket ranges, counts and tallies in the snapshot are
  // consistent with the bucket ranges and checksums in our histogram.  This can
  // produce a false-alarm if a race occurred in the reading of the data during
  // a SnapShot process, but should otherwise be false at all times (unless we
  // have memory over-writes, or DRAM failures). Flag definitions are located
  // under "enum Inconsistency" in base/metrics/histogram_base.h.
  uint32_t FindCorruption(const HistogramSamples& samples) const override;

  //----------------------------------------------------------------------------
  // Accessors for factory construction, serialization and testing.
  //----------------------------------------------------------------------------
  const BucketRanges* bucket_ranges() const;
  Sample declared_min() const;
  Sample declared_max() const;
  virtual Sample ranges(uint32_t i) const;
  virtual uint32_t bucket_count() const;

  // This function validates histogram construction arguments. It returns false
  // if some of the arguments are bad but also corrects them so they should
  // function on non-dcheck builds without crashing.
  // Note. Currently it allow some bad input, e.g. 0 as minimum, but silently
  // converts it to good input: 1.
  // TODO(bcwhite): Use false returns to create "sink" histograms so that bad
  // data doesn't create confusion on the servers.
  static bool InspectConstructionArguments(StringPiece name,
                                           Sample* minimum,
                                           Sample* maximum,
                                           uint32_t* bucket_count);

  // HistogramBase implementation:
  uint64_t name_hash() const override;
  HistogramType GetHistogramType() const override;
  bool HasConstructionArguments(Sample expected_minimum,
                                Sample expected_maximum,
                                uint32_t expected_bucket_count) const override;
  void Add(Sample value) override;
  void AddCount(Sample value, int count) override;
  std::unique_ptr<HistogramSamples> SnapshotSamples() const override;
  std::unique_ptr<HistogramSamples> SnapshotDelta() override;
  std::unique_ptr<HistogramSamples> SnapshotFinalDelta() const override;
  void AddSamples(const HistogramSamples& samples) override;
  bool AddSamplesFromPickle(base::PickleIterator* iter) override;
  void WriteHTMLGraph(std::string* output) const override;
  void WriteAscii(std::string* output) const override;

  // Validates the histogram contents and CHECKs on errors.
  // TODO(bcwhite): Remove this after https://crbug/836875.
  void ValidateHistogramContents() const override;

 protected:
  // This class, defined entirely within the .cc file, contains all the
  // common logic for building a Histogram and can be overridden by more
  // specific types to alter details of how the creation is done. It is
  // defined as an embedded class (rather than an anonymous one) so it
  // can access the protected constructors.
  class Factory;

  // |ranges| should contain the underflow and overflow buckets. See top
  // comments for example.
  Histogram(const char* name,
            Sample minimum,
            Sample maximum,
            const BucketRanges* ranges);

  // Traditionally, histograms allocate their own memory for the bucket
  // vector but "shared" histograms use memory regions allocated from a
  // special memory segment that is passed in here.  It is assumed that
  // the life of this memory is managed externally and exceeds the lifetime
  // of this object. Practically, this memory is never released until the
  // process exits and the OS cleans it up.
  Histogram(const char* name,
            Sample minimum,
            Sample maximum,
            const BucketRanges* ranges,
            const DelayedPersistentAllocation& counts,
            const DelayedPersistentAllocation& logged_counts,
            HistogramSamples::Metadata* meta,
            HistogramSamples::Metadata* logged_meta);

  // HistogramBase implementation:
  void SerializeInfoImpl(base::Pickle* pickle) const override;

  // Method to override to skip the display of the i'th bucket if it's empty.
  virtual bool PrintEmptyBucket(uint32_t index) const;

  // Get normalized size, relative to the ranges(i).
  virtual double GetBucketSize(Count current, uint32_t i) const;

  // Return a string description of what goes in a given bucket.
  // Most commonly this is the numeric value, but in derived classes it may
  // be a name (or string description) given to the bucket.
  virtual const std::string GetAsciiBucketRange(uint32_t it) const;

 private:
  // Allow tests to corrupt our innards for testing purposes.
  FRIEND_TEST_ALL_PREFIXES(HistogramTest, BoundsTest);
  FRIEND_TEST_ALL_PREFIXES(HistogramTest, BucketPlacementTest);
  FRIEND_TEST_ALL_PREFIXES(HistogramTest, CorruptSampleCounts);

  friend class StatisticsRecorder;  // To allow it to delete duplicates.
  friend class StatisticsRecorderTest;

  friend BASE_EXPORT HistogramBase* DeserializeHistogramInfo(
      base::PickleIterator* iter);
  static HistogramBase* DeserializeInfoImpl(base::PickleIterator* iter);

  // Create a snapshot containing all samples (both logged and unlogged).
  // Implementation of SnapshotSamples method with a more specific type for
  // internal use.
  std::unique_ptr<SampleVector> SnapshotAllSamples() const;

  // Create a copy of unlogged samples.
  std::unique_ptr<SampleVector> SnapshotUnloggedSamples() const;

  //----------------------------------------------------------------------------
  // Helpers for emitting Ascii graphic.  Each method appends data to output.

  void WriteAsciiImpl(bool graph_it,
                      const std::string& newline,
                      std::string* output) const;

  // Find out how large (graphically) the largest bucket will appear to be.
  double GetPeakBucketSize(const SampleVectorBase& samples) const;

  // Write a common header message describing this histogram.
  void WriteAsciiHeader(const SampleVectorBase& samples,
                        Count sample_count,
                        std::string* output) const;

  // Write information about previous, current, and next buckets.
  // Information such as cumulative percentage, etc.
  void WriteAsciiBucketContext(const int64_t past,
                               const Count current,
                               const int64_t remaining,
                               const uint32_t i,
                               std::string* output) const;

  // WriteJSON calls these.
  void GetParameters(DictionaryValue* params) const override;

  void GetCountAndBucketData(Count* count,
                             int64_t* sum,
                             ListValue* buckets) const override;

  // Samples that have not yet been logged with SnapshotDelta().
  std::unique_ptr<SampleVectorBase> unlogged_samples_;

  // Accumulation of all samples that have been logged with SnapshotDelta().
  std::unique_ptr<SampleVectorBase> logged_samples_;

#if DCHECK_IS_ON()  // Don't waste memory if it won't be used.
  // Flag to indicate if PrepareFinalDelta has been previously called. It is
  // used to DCHECK that a final delta is not created multiple times.
  mutable bool final_delta_created_ = false;
#endif

  DISALLOW_COPY_AND_ASSIGN(Histogram);
};

//------------------------------------------------------------------------------

// LinearHistogram is a more traditional histogram, with evenly spaced
// buckets.
class BASE_EXPORT LinearHistogram : public Histogram {
 public:
  ~LinearHistogram() override;

  /* minimum should start from 1. 0 is as minimum is invalid. 0 is an implicit
     default underflow bucket. */
  static HistogramBase* FactoryGet(const std::string& name,
                                   Sample minimum,
                                   Sample maximum,
                                   uint32_t bucket_count,
                                   int32_t flags);
  static HistogramBase* FactoryTimeGet(const std::string& name,
                                       TimeDelta minimum,
                                       TimeDelta maximum,
                                       uint32_t bucket_count,
                                       int32_t flags);

  // Overloads of the above two functions that take a const char* |name| param,
  // to avoid code bloat from the std::string constructor being inlined into
  // call sites.
  static HistogramBase* FactoryGet(const char* name,
                                   Sample minimum,
                                   Sample maximum,
                                   uint32_t bucket_count,
                                   int32_t flags);
  static HistogramBase* FactoryTimeGet(const char* name,
                                       TimeDelta minimum,
                                       TimeDelta maximum,
                                       uint32_t bucket_count,
                                       int32_t flags);

  // Create a histogram using data in persistent storage.
  static std::unique_ptr<HistogramBase> PersistentCreate(
      const char* name,
      Sample minimum,
      Sample maximum,
      const BucketRanges* ranges,
      const DelayedPersistentAllocation& counts,
      const DelayedPersistentAllocation& logged_counts,
      HistogramSamples::Metadata* meta,
      HistogramSamples::Metadata* logged_meta);

  struct DescriptionPair {
    Sample sample;
    const char* description;  // Null means end of a list of pairs.
  };

  // Create a LinearHistogram and store a list of number/text values for use in
  // writing the histogram graph.
  // |descriptions| can be NULL, which means no special descriptions to set. If
  // it's not NULL, the last element in the array must has a NULL in its
  // "description" field.
  static HistogramBase* FactoryGetWithRangeDescription(
      const std::string& name,
      Sample minimum,
      Sample maximum,
      uint32_t bucket_count,
      int32_t flags,
      const DescriptionPair descriptions[]);

  static void InitializeBucketRanges(Sample minimum,
                                     Sample maximum,
                                     BucketRanges* ranges);

  // Overridden from Histogram:
  HistogramType GetHistogramType() const override;

 protected:
  class Factory;

  LinearHistogram(const char* name,
                  Sample minimum,
                  Sample maximum,
                  const BucketRanges* ranges);

  LinearHistogram(const char* name,
                  Sample minimum,
                  Sample maximum,
                  const BucketRanges* ranges,
                  const DelayedPersistentAllocation& counts,
                  const DelayedPersistentAllocation& logged_counts,
                  HistogramSamples::Metadata* meta,
                  HistogramSamples::Metadata* logged_meta);

  double GetBucketSize(Count current, uint32_t i) const override;

  // If we have a description for a bucket, then return that.  Otherwise
  // let parent class provide a (numeric) description.
  const std::string GetAsciiBucketRange(uint32_t i) const override;

  // Skip printing of name for numeric range if we have a name (and if this is
  // an empty bucket).
  bool PrintEmptyBucket(uint32_t index) const override;

 private:
  friend BASE_EXPORT HistogramBase* DeserializeHistogramInfo(
      base::PickleIterator* iter);
  static HistogramBase* DeserializeInfoImpl(base::PickleIterator* iter);

  // For some ranges, we store a printable description of a bucket range.
  // If there is no description, then GetAsciiBucketRange() uses parent class
  // to provide a description.
  typedef std::map<Sample, std::string> BucketDescriptionMap;
  BucketDescriptionMap bucket_description_;

  DISALLOW_COPY_AND_ASSIGN(LinearHistogram);
};

//------------------------------------------------------------------------------

// BooleanHistogram is a histogram for booleans.
class BASE_EXPORT BooleanHistogram : public LinearHistogram {
 public:
  static HistogramBase* FactoryGet(const std::string& name, int32_t flags);

  // Overload of the above function that takes a const char* |name| param,
  // to avoid code bloat from the std::string constructor being inlined into
  // call sites.
  static HistogramBase* FactoryGet(const char* name, int32_t flags);

  // Create a histogram using data in persistent storage.
  static std::unique_ptr<HistogramBase> PersistentCreate(
      const char* name,
      const BucketRanges* ranges,
      const DelayedPersistentAllocation& counts,
      const DelayedPersistentAllocation& logged_counts,
      HistogramSamples::Metadata* meta,
      HistogramSamples::Metadata* logged_meta);

  HistogramType GetHistogramType() const override;

 protected:
  class Factory;

 private:
  BooleanHistogram(const char* name, const BucketRanges* ranges);
  BooleanHistogram(const char* name,
                   const BucketRanges* ranges,
                   const DelayedPersistentAllocation& counts,
                   const DelayedPersistentAllocation& logged_counts,
                   HistogramSamples::Metadata* meta,
                   HistogramSamples::Metadata* logged_meta);

  friend BASE_EXPORT HistogramBase* DeserializeHistogramInfo(
      base::PickleIterator* iter);
  static HistogramBase* DeserializeInfoImpl(base::PickleIterator* iter);

  DISALLOW_COPY_AND_ASSIGN(BooleanHistogram);
};

//------------------------------------------------------------------------------

// CustomHistogram is a histogram for a set of custom integers.
class BASE_EXPORT CustomHistogram : public Histogram {
 public:
  // |custom_ranges| contains a vector of limits on ranges. Each limit should be
  // > 0 and < kSampleType_MAX. (Currently 0 is still accepted for backward
  // compatibility). The limits can be unordered or contain duplication, but
  // client should not depend on this.
  static HistogramBase* FactoryGet(const std::string& name,
                                   const std::vector<Sample>& custom_ranges,
                                   int32_t flags);

  // Overload of the above function that takes a const char* |name| param,
  // to avoid code bloat from the std::string constructor being inlined into
  // call sites.
  static HistogramBase* FactoryGet(const char* name,
                                   const std::vector<Sample>& custom_ranges,
                                   int32_t flags);

  // Create a histogram using data in persistent storage.
  static std::unique_ptr<HistogramBase> PersistentCreate(
      const char* name,
      const BucketRanges* ranges,
      const DelayedPersistentAllocation& counts,
      const DelayedPersistentAllocation& logged_counts,
      HistogramSamples::Metadata* meta,
      HistogramSamples::Metadata* logged_meta);

  // Overridden from Histogram:
  HistogramType GetHistogramType() const override;

  // Helper method for transforming an array of valid enumeration values
  // to the std::vector<int> expected by UMA_HISTOGRAM_CUSTOM_ENUMERATION.
  // This function ensures that a guard bucket exists right after any
  // valid sample value (unless the next higher sample is also a valid value),
  // so that invalid samples never fall into the same bucket as valid samples.
  static std::vector<Sample> ArrayToCustomEnumRanges(
      base::span<const Sample> values);

 protected:
  class Factory;

  CustomHistogram(const char* name, const BucketRanges* ranges);

  CustomHistogram(const char* name,
                  const BucketRanges* ranges,
                  const DelayedPersistentAllocation& counts,
                  const DelayedPersistentAllocation& logged_counts,
                  HistogramSamples::Metadata* meta,
                  HistogramSamples::Metadata* logged_meta);

  // HistogramBase implementation:
  void SerializeInfoImpl(base::Pickle* pickle) const override;

  double GetBucketSize(Count current, uint32_t i) const override;

 private:
  friend BASE_EXPORT HistogramBase* DeserializeHistogramInfo(
      base::PickleIterator* iter);
  static HistogramBase* DeserializeInfoImpl(base::PickleIterator* iter);

  static bool ValidateCustomRanges(const std::vector<Sample>& custom_ranges);

  DISALLOW_COPY_AND_ASSIGN(CustomHistogram);
};

}  // namespace base

#endif  // BASE_METRICS_HISTOGRAM_H_
