#ifndef MAP_API_BENCHMARKS_KMEANS_VIEW_H_
#define MAP_API_BENCHMARKS_KMEANS_VIEW_H_

#include <map-api/cr-table.h>
#include <map-api/transaction.h>

#include "map_api_benchmarks/common.h"

namespace map_api {
namespace benchmarks{

/**
 * Stores the current view defined by a transaction on specified chunks.
 * Holds the revision cache. Provides interface to commit, and,
 * multi-kmeans specifically, to update only changes related to a specified
 * center.
 */
class KmeansView {
 public:
  KmeansView(Chunk* descriptor_chunk_id, Chunk* center_chunk_id,
             Chunk* membership_chunk_id, Transaction* transaction);

  /**
   * Does not populate the local revision maps but pushes on straight to the
   * transaction.
   */
  void insert(const DescriptorVector& descriptors,
              const DescriptorVector& centers,
              const std::vector<unsigned int>& memberships);

  void fetch(DescriptorVector* descriptors,
             DescriptorVector* centers,
             std::vector<unsigned int>* memberships);

  void updateCenterRelated(size_t chosen_center,
                           const DescriptorVector& descriptors,
                           const DescriptorVector& centers,
                           const std::vector<unsigned int>& memberships);
 private:
  CRTable::RevisionMap descriptor_revisions_;
  CRTable::RevisionMap center_revisions_;
  CRTable::RevisionMap membership_revisions_;

  std::unordered_map<size_t, Id> descriptor_index_to_id_;
  std::unordered_map<Id, size_t> descriptor_id_to_index_;

  std::unordered_map<size_t, Id> center_index_to_id_;
  std::unordered_map<Id, size_t> center_id_to_index_;

  Chunk* descriptor_chunk_, *center_chunk_, *membership_chunk_;
  map_api::Transaction* transaction_;
};

} /* namespace benchmarks */
} /* namespace map_api */

#endif /* MAP_API_BENCHMARKS_KMEANS_VIEW_H_ */
