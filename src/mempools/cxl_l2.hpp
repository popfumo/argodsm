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

class l2_stats
{
public:
    std::atomic<std::size_t> hits{0};
    std::atomic<std::size_t> misses{0};
    std::atomic<std::size_t> inserts{0};
    std::atomic<std::size_t> evictions{0};
    std::atomic<std::size_t> bytes_l1_to_l2{0};
    std::atomic<std::size_t> bytes_l2_to_l1{0};
    std::atomic<std::size_t> remote_pages_inserted{0};
    std::atomic<std::size_t> remote_pages_evicted{0};
    void l2_reset_stats()
    {
        hits.store(0, std::memory_order_relaxed);
        misses.store(0, std::memory_order_relaxed);
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
        };

        /**
         * @brief Get the L2 cache index for a given aligned address
         * @param aligned_addr The aligned global address (must be aligned to PAGE_SIZE*CACHELINE)
         * @param l2_entries Total number of entries in the L2 cache
         * @return The cache index
         */
        inline std::size_t getL2CacheIndex(std::uintptr_t aligned_addr, std::size_t l2_entries)
        {
            const std::size_t block_size = PAGE_SIZE * CACHELINE;
            return (aligned_addr / block_size) % l2_entries;
        }

        /**
         * @brief Initialize L2 control data array on CXL memory (NUMA node 2)
         * @param num_entries Number of cache entries to allocate
         * @return Pointer to the allocated control array
         */
        l2_control_data *l2_control_init(std::size_t num_entries)
        {
            void *ptr = numa_alloc_onnode(num_entries * sizeof(l2_control_data), 2);
            if (!ptr)
            {
                throw std::runtime_error("Failed to allocate CXL L2 cache control memory");
            }
            memset(ptr, 0, num_entries * sizeof(l2_control_data));

            // Initialize all entries to INVALID
            l2_control_data *controls = static_cast<l2_control_data *>(ptr);
            for (std::size_t i = 0; i < num_entries; ++i)
            {
                controls[i].state = INVALID;
                controls[i].dirty = CLEAN;
                controls[i].tag = 0;
            }

            printf("L2 control initialized with %zu entries (%zu bytes)\n",
                   num_entries, num_entries * sizeof(l2_control_data));
            return controls;
        }

        /**
         * @brief Initialize L2 data array on CXL memory (NUMA node 2)
         * @param size_bytes Total size in bytes for L2 cache data
         * @return Pointer to the allocated data array
         */
        char *l2Data_init(std::size_t size_bytes)
        {
            void *ptr = numa_alloc_onnode(size_bytes, 2);
            if (!ptr)
            {
                throw std::runtime_error("Failed to allocate CXL L2 cache data memory");
            }
            memset(ptr, 0, size_bytes);
            printf("L2 data cache initialized with %zu bytes\n", size_bytes);
            return static_cast<char *>(ptr);
        }

        /**
         * @brief Free L2 cache memory
         * @param ptr Pointer to the allocated memory
         * @param size Size of the allocated memory
         */
        void l2_free(void *ptr, std::size_t size)
        {
            numa_free(ptr, size);
        }

        /**
         * @brief Look up a page in the L2 cache
         * @param l2_controls Pointer to the L2 control data array
         * @param num_entries Total number of entries in the L2 cache
         * @param aligned_addr The aligned global address to look up
         * @param index_out Output parameter for the cache index if found
         * @return true if page is in L2 and valid, false otherwise
         */
        bool l2_lookup(l2_control_data *l2_controls, std::size_t num_entries,
                       std::uintptr_t aligned_addr, std::size_t &index_out, int workrank)
        {
            assert(l2_controls != nullptr);

            std::size_t idx = getL2CacheIndex(aligned_addr, num_entries);

            if (l2_controls[idx].tag == aligned_addr && l2_controls[idx].state == VALID)
            {
                index_out = idx;
                printf("Node %d L2 cache HIT for addr %lu at index %zu\n", workrank, aligned_addr, idx);
                return true;
            }

            printf("Node %d L2 cache MISS for addr %lu at index %zu (tag=%lu, state=%d)\n", workrank,
                   aligned_addr, idx, l2_controls[idx].tag, l2_controls[idx].state);
            return false;
        }

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
                       void *page_src, bool is_dirty, int workrank)
        {
            assert(l2_controls != nullptr);
            assert(l2_data != nullptr);
            assert(page_src != nullptr);
            const std::size_t block_size = PAGE_SIZE * CACHELINE;

            std::size_t idx = getL2CacheIndex(aligned_addr, num_entries);

            // If evicting an existing valid entry, count it
            if (l2_controls[idx].state == VALID && l2_controls[idx].tag != aligned_addr)
            {
                printf("Node %d L2 evicting addr %lu from index %zu to make room for %lu\n", workrank,
                       l2_controls[idx].tag, idx, aligned_addr);
            }

            // Copy the cache block (CACHELINE pages) to L2
            void *dest = l2_data + (idx * block_size);
            memcpy(dest, page_src, block_size);

            // Update control data
            l2_controls[idx].tag = aligned_addr;
            l2_controls[idx].state = VALID;
            l2_controls[idx].dirty = is_dirty ? DIRTY : CLEAN;

            printf("Node %d L2 inserted addr %lu at index %zu (dest=%p)\n", workrank, aligned_addr, idx, dest);
        }

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
                        char *l2_data, std::uintptr_t aligned_addr, void *page_dest, int workrank)
        {
            assert(l2_controls != nullptr);
            assert(l2_data != nullptr);
            assert(page_dest != nullptr);
            const std::size_t block_size = PAGE_SIZE * CACHELINE;

            std::size_t idx;
            if (!l2_lookup(l2_controls, num_entries, aligned_addr, idx, workrank))
            {
                printf("L2 extract failed: addr %lu not found\n", aligned_addr);
                return false;
            }

            // Copy the cache block from L2 to destination
            void *src = l2_data + (idx * block_size);
            memcpy(page_dest, src, block_size);

            printf("L2 extracted addr %lu from index %zu to dest %p\n", aligned_addr, idx, page_dest);
            return true;
        }

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
                               bool &victim_dirty)
        {
            assert(l2_controls != nullptr);

            std::size_t idx = getL2CacheIndex(aligned_addr, num_entries);

            if (l2_controls[idx].state == VALID && l2_controls[idx].tag != aligned_addr)
            {
                victim_addr = l2_controls[idx].tag;
                victim_dirty = (l2_controls[idx].dirty == DIRTY);
                return true;
            }
            return false;
        }

        /**
         * @brief Get L2 cache data pointer for a given index
         * @param l2_data Pointer to the L2 data array
         * @param num_entries Total number of entries in the L2 cache
         * @param aligned_addr The aligned global address
         * @return Pointer to the cache data for that index
         */
        void *l2_get_data_ptr(char *l2_data, std::size_t num_entries, std::uintptr_t aligned_addr)
        {
            const std::size_t block_size = PAGE_SIZE * CACHELINE;
            std::size_t idx = getL2CacheIndex(aligned_addr, num_entries);
            return l2_data + (idx * block_size);
        }

        /**
         * @brief Mark L2 entry as clean after writeback
         * @param l2_controls Pointer to the L2 control data array
         * @param num_entries Total number of entries in the L2 cache
         * @param aligned_addr The aligned global address
         */
        void l2_mark_clean(l2_control_data *l2_controls, std::size_t num_entries,
                           std::uintptr_t aligned_addr)
        {
            assert(l2_controls != nullptr);
            std::size_t idx = getL2CacheIndex(aligned_addr, num_entries);

            if (l2_controls[idx].tag == aligned_addr && l2_controls[idx].state == VALID)
            {
                l2_controls[idx].dirty = CLEAN;
            }
        }

        /**
         * @brief Get dirty status of an L2 entry
         * @param l2_controls Pointer to the L2 control data array
         * @param num_entries Total number of entries in the L2 cache
         * @param aligned_addr The aligned global address
         * @return true if entry is valid and dirty
         */
        bool l2_is_dirty(l2_control_data *l2_controls, std::size_t num_entries,
                         std::uintptr_t aligned_addr)
        {
            assert(l2_controls != nullptr);
            std::size_t idx = getL2CacheIndex(aligned_addr, num_entries);

            return (l2_controls[idx].tag == aligned_addr &&
                    l2_controls[idx].state == VALID &&
                    l2_controls[idx].dirty == DIRTY);
        }

        /**
         * @brief Invalidate an L2 cache entry
         * @param l2_controls Pointer to the L2 control data array
         * @param num_entries Total number of entries in the L2 cache
         * @param aligned_addr The aligned global address to invalidate
         */
        void l2_invalidate(l2_control_data *l2_controls, std::size_t num_entries,
                           std::uintptr_t aligned_addr)
        {
            assert(l2_controls != nullptr);

            std::size_t idx = getL2CacheIndex(aligned_addr, num_entries);

            if (l2_controls[idx].tag == aligned_addr && l2_controls[idx].state == VALID)
            {
                l2_controls[idx].state = INVALID;
                printf("L2 invalidated addr %lu at index %zu\n", aligned_addr, idx);
            }
        }

    }
}