#include "cxl_l2.hpp"


namespace argo
{
    namespace cxl_l2
    {
        //#define PRINT_L2

        inline std::size_t get_l2_cache_index(std::uintptr_t aligned_addr, std::size_t l2_entries)
        {
            const std::size_t block_size = PAGE_SIZE * CACHELINE;
            return (aligned_addr / block_size) % l2_entries;
        }

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

        void l2_free(void *ptr, std::size_t size)
        {
            numa_free(ptr, size);
        }

        bool l2_lookup(l2_control_data *l2_controls, std::size_t num_entries,
                       std::uintptr_t aligned_addr, std::size_t &index_out, int workrank)
        {
            assert(l2_controls != nullptr);

            std::size_t idx = get_l2_cache_index(aligned_addr, num_entries);

            if (l2_controls[idx].tag == aligned_addr && l2_controls[idx].state == VALID)
            {
                index_out = idx;
                #ifdef PRINT_L2
                printf("Node %d L2 cache HIT for addr %lu at index %zu\n", workrank, aligned_addr, idx);
                #endif
                #ifndef PRINT_L2
                (void)workrank; // Why
                #endif
                return true;
            }

            #ifdef PRINT_L2
            printf("Node %d L2 cache MISS for addr %lu at index %zu (tag=%lu, state=%d)\n", workrank,
                   aligned_addr, idx, l2_controls[idx].tag, l2_controls[idx].state);
            #endif
            return false;
        }

        void l2_insert(l2_control_data *l2_controls, std::size_t num_entries,
                       char *l2_data, std::uintptr_t aligned_addr,
                       void *page_src, bool is_dirty, int workrank)
        {
            assert(l2_controls != nullptr);
            assert(l2_data != nullptr);
            assert(page_src != nullptr);
            const std::size_t block_size = PAGE_SIZE * CACHELINE;

            std::size_t idx = get_l2_cache_index(aligned_addr, num_entries);

            // If evicting an existing valid entry, count it
            if (l2_controls[idx].state == VALID && l2_controls[idx].tag != aligned_addr)
            {
                #ifdef PRINT_L2
                printf("Node %d L2 evicting addr %lu from index %zu to make room for %lu\n", workrank,
                       l2_controls[idx].tag, idx, aligned_addr);
                #endif
                #ifndef PRINT_L2
                (void)workrank;
                #endif
            }

            // Copy the cache block (CACHELINE pages) to L2
            void *dest = l2_data + (idx * block_size);
            memcpy(dest, page_src, block_size);

            // Update control data
            l2_controls[idx].tag = aligned_addr;
            l2_controls[idx].state = VALID;
            l2_controls[idx].dirty = is_dirty ? DIRTY : CLEAN;
            #ifdef PRINT_L2
            printf("Node %d L2 inserted addr %lu at index %zu (dest=%p)\n", workrank, aligned_addr, idx, dest);
            #endif
        }

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
                #ifdef PRINT_L2
                printf("L2 extract failed: addr %lu not found\n", aligned_addr);
                #endif
                return false;
            }

            // Copy the cache block from L2 to destination
            void *src = l2_data + (idx * block_size);
            memcpy(page_dest, src, block_size);
            #ifdef PRINT_L2
            printf("L2 extracted addr %lu from index %zu to dest %p\n", aligned_addr, idx, page_dest);
            #endif 
            return true;
        }

        bool l2_needs_eviction(l2_control_data *l2_controls, std::size_t num_entries,
                               std::uintptr_t aligned_addr, std::uintptr_t &victim_addr,
                               bool &victim_dirty)
        {
            assert(l2_controls != nullptr);
            assert(aligned_addr != NULL);
            assert(victim_addr != NULL);
            std::size_t idx = get_l2_cache_index(aligned_addr, num_entries);

            if (l2_controls[idx].state == VALID && l2_controls[idx].tag != aligned_addr)
            {
                victim_addr = l2_controls[idx].tag;
                victim_dirty = (l2_controls[idx].dirty == DIRTY);
                return true;
            }
            return false;
        }

        void *l2_get_data_ptr(char *l2_data, std::size_t num_entries, std::uintptr_t aligned_addr)
        {
            const std::size_t block_size = PAGE_SIZE * CACHELINE;
            std::size_t idx = get_l2_cache_index(aligned_addr, num_entries);
            return l2_data + (idx * block_size);
        }

        void l2_mark_clean(l2_control_data *l2_controls, std::size_t num_entries,
                           std::uintptr_t aligned_addr)
        {
            assert(l2_controls != nullptr);
            std::size_t idx = get_l2_cache_index(aligned_addr, num_entries);

            if (l2_controls[idx].tag == aligned_addr && l2_controls[idx].state == VALID)
            {
                l2_controls[idx].dirty = CLEAN;
            }
        }

        bool l2_is_dirty(l2_control_data *l2_controls, std::size_t num_entries,
                         std::uintptr_t aligned_addr)
        {
            assert(l2_controls != nullptr);
            std::size_t idx = get_l2_cache_index(aligned_addr, num_entries);

            return (l2_controls[idx].tag == aligned_addr &&
                    l2_controls[idx].state == VALID &&
                    l2_controls[idx].dirty == DIRTY);
        }

        void l2_invalidate(l2_control_data *l2_controls, std::size_t num_entries,
                           std::uintptr_t aligned_addr)
        {
            assert(l2_controls != nullptr);

            std::size_t idx = get_l2_cache_index(aligned_addr, num_entries);

            if (l2_controls[idx].tag == aligned_addr && l2_controls[idx].state == VALID)
            {
                l2_controls[idx].state = INVALID;
                #ifdef PRINT_L2
                printf("L2 invalidated addr %lu at index %zu\n", aligned_addr, idx);
                #endif
            }
        }

    }
}