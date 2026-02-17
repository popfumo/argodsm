/*
  Prototype for L2 cache handling functions
*/

#include <iostream>
#include <memory>
#include "backend/backend.hpp"
#include "config.hpp"
#include "data_distribution/global_ptr.hpp"
#include "synchronization/global_tas_lock.hpp"
#include <numa.h>
#include <numaif.h>
#include "backend/mpi/swdsm.h"

#pragma once
class l2_stats
{
public:
    std::atomic<std::size_t> inserts{0};
    std::atomic<std::size_t> evictions{0};
    std::atomic<std::size_t> bytes_l1_to_l2{0};
    std::atomic<std::size_t> bytes_l2_to_l1{0};
    std::atomic<std::size_t> remote_pages_inserted{0};
    std::atomic<std::size_t> remote_pages_evicted{0};
    
    void l2_reset_stats()
    {
        inserts.store(0, std::memory_order_relaxed);
        evictions.store(0, std::memory_order_relaxed);
        bytes_l1_to_l2.store(0, std::memory_order_relaxed);
        bytes_l2_to_l1.store(0, std::memory_order_relaxed);
        remote_pages_inserted.store(0, std::memory_order_relaxed);
        remote_pages_evicted.store(0, std::memory_order_relaxed);
    }
};

namespace argo
{
    namespace cxl_l2
    {
        struct l2_control_data
        {
            /** @brief State of the cache line */
            argo_byte state;
            /** @brief Dirty bit of the cache line */
            argo_byte dirty;
            /** @brief Tag of the cache line - stores the aligned global address */
            std::uintptr_t tag;
            std::mutex lock; 
        };

        /**
         * @brief Get the L2 cache index for a given aligned address
         * @param aligned_addr The aligned global address (must be aligned to PAGE_SIZE*CACHELINE)
         * @param l2_entries Total number of entries in the L2 cache
         * @return The cache index
         */
        inline std::size_t get_l2_cache_index(std::uintptr_t aligned_addr, std::size_t l2_entries);

        /**
         * @brief Initialize L2 control data array on CXL memory (NUMA node 2)
         * @param num_entries Number of cache entries to allocate
         * @return Pointer to the allocated control array
         */
        l2_control_data *l2_control_init(std::size_t num_entries);

        /**
         * @brief Initialize L2 data array on CXL memory (NUMA node 2)
         * @param size_bytes Total size in bytes for L2 cache data
         * @return Pointer to the allocated data array
         */
        char *l2Data_init(std::size_t size_bytes);

        /**
         * @brief Free L2 cache memory
         * @param ptr Pointer to the allocated memory
         * @param size Size of the allocated memory
         */
        void l2_free(void *ptr, std::size_t size);

        /**
         * @brief Look up a page in the L2 cache
         * @param l2_controls Pointer to the L2 control data array
         * @param num_entries Total number of entries in the L2 cache
         * @param aligned_addr The aligned global address to look up
         * @param index_out Output parameter for the cache index if found
         * @return true if page is in L2 and valid, false otherwise
         */
        bool l2_lookup(l2_control_data *l2_controls, std::size_t num_entries,
                       std::uintptr_t aligned_addr, std::size_t &index_out, int workrank);

        /**
         * @brief Insert a page into the L2 cache
         * @param l2_controls Pointer to the L2 control data array
         * @param num_entries Total number of entries in the L2 cache
         * @param l2_data Pointer to the L2 data array
         * @param aligned_addr The aligned global address being inserted
         * @param page_src Pointer to the source data (L1 cache entry)
         * @param is_dirty Whether the page is dirty
         */
        void l2_insert(l2_control_data *l2_controls, std::size_t num_entries,
                       char *l2_data, std::uintptr_t aligned_addr,
                       void *page_src, bool is_dirty, int workrank);

        /**
         * @brief Extract a page from L2 cache back to L1
         * @param l2_controls Pointer to the L2 control data array
         * @param num_entries Total number of entries in the L2 cache
         * @param l2_data Pointer to the L2 data array
         * @param aligned_addr The aligned global address to extract
         * @param page_dest Destination pointer (L1 cache entry)
         * @return true if successfully extracted, false if not found
         */
        bool l2_extract(l2_control_data *l2_controls, std::size_t num_entries,
                        char *l2_data, std::uintptr_t aligned_addr, void *page_dest, int workrank);

        /**
         * @brief Check if inserting at aligned_addr would require evicting a valid entry
         * @param l2_controls Pointer to the L2 control data array
         * @param num_entries Total number of entries in the L2 cache
         * @param aligned_addr The aligned global address to be inserted
         * @param victim_addr Output: the address of the victim entry (if any)
         * @param victim_dirty Output: whether the victim is dirty
         * @return true if eviction is needed (valid entry with different tag exists)
         */
        bool l2_needs_eviction(l2_control_data *l2_controls, std::size_t num_entries,
                               std::uintptr_t aligned_addr, std::uintptr_t &victim_addr,
                               bool &victim_dirty);

        /**
         * @brief Get L2 cache data pointer for a given index
         * @param l2_data Pointer to the L2 data array
         * @param num_entries Total number of entries in the L2 cache
         * @param aligned_addr The aligned global address
         * @return Pointer to the cache data for that index
         */
        void *l2_get_data_ptr(char *l2_data, std::size_t num_entries, std::uintptr_t aligned_addr);

        /**
         * @brief Mark L2 entry as clean after writeback
         * @param l2_controls Pointer to the L2 control data array
         * @param num_entries Total number of entries in the L2 cache
         * @param aligned_addr The aligned global address
         */
        void l2_mark_clean(l2_control_data *l2_controls, std::size_t num_entries,
                           std::uintptr_t aligned_addr);

        /**
         * @brief Get dirty status of an L2 entry
         * @param l2_controls Pointer to the L2 control data array
         * @param num_entries Total number of entries in the L2 cache
         * @param aligned_addr The aligned global address
         * @return true if entry is valid and dirty
         */
        bool l2_is_dirty(l2_control_data *l2_controls, std::size_t num_entries,
                         std::uintptr_t aligned_addr);

        /**
         * @brief Invalidate an L2 cache entry
         * @param l2_controls Pointer to the L2 control data array
         * @param num_entries Total number of entries in the L2 cache
         * @param aligned_addr The aligned global address to invalidate
         */
        void l2_invalidate(l2_control_data *l2_controls, std::size_t num_entries,
                           std::uintptr_t aligned_addr);

    }
}