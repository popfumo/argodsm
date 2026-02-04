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
    void l2_reset_stats()
    {
        hits.store(0, std::memory_order_relaxed);
        misses.store(0, std::memory_order_relaxed);
        inserts.store(0, std::memory_order_relaxed);
        evictions.store(0, std::memory_order_relaxed);
        bytes_l1_to_l2.store(0, std::memory_order_relaxed);
        bytes_l2_to_l1.store(0, std::memory_order_relaxed);
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
            /** @brief Tag of the cache line */
            std::uintptr_t tag;
            std::size_t size = 0;
            std::size_t pos = 0;
        };

        struct cxl_l2_info
        {
            void *base = nullptr;
            std::size_t size = 0;
            std::size_t pos = 0;
        };

        // Current plan: Just allocate fixed memory for L2 cache on the cxl memory module (numa node 2)
        // and then write some data to it when evicting from L1 cache.

        l2_control_data *l2_control_init(std::size_t size)
        {
            void *ptr = numa_alloc_onnode(size * sizeof(l2_control_data), 2);
            if (!ptr)
            {
                throw std::runtime_error("Failed to allocate CXL L2 cache control memory");
            }
            memset(ptr, 0, size * sizeof(l2_control_data));
            static_cast<l2_control_data *>(ptr)->size = size;
            return static_cast<l2_control_data *>(ptr);
        }

        char *l2Data_init(std::size_t size)
        {
            void *ptr = numa_alloc_onnode(size, 2);
            if (!ptr)
            {
                throw std::runtime_error("Failed to allocate CXL L2 cache memory");
            }
            memset(ptr, 0, size);
            printf("L2 cache initialized with size %lu bytes\n", (unsigned long)size);

            return static_cast<char *>(ptr);
        }

        cxl_l2_info *l2_init(std::size_t size)
        {
            void *ptr = numa_alloc_onnode(size, 2);
            if (!ptr)
            {
                throw std::runtime_error("Failed to allocate CXL L2 cache memory");
            }
            memset(ptr, 0, size);

            return new cxl_l2_info{ptr, size, 0};
        }

        void l2_free(void *ptr, std::size_t size)
        {
            numa_free(ptr, size);
        }

        void l2_insert(l2_control_data &l2_control, char *l2_data, void *page_src)
        {
            // Insert page into L2 cache
            // When page is inserted, move pointer forward in circular manner
            // If no space left, evict oldest page (just overwrite it for now)

            // need to somehow check where to write, meaning we need to find the current offset in L2 cache

            // Insert a page at the current position
            // need to introduce some sort of tag that tells where the page is stored in L2 cache
            assert(l2_data != nullptr);
            assert(page_src != nullptr);
            assert(l2_control.pos + PAGE_SIZE <= l2_control.size);
            printf("Inserting page into L2 cache at position %zu, address %p\n", l2_control.pos, l2_data + l2_control.pos);
            printf("Page source address: %p\n", page_src);
            void *dest = l2_data + l2_control.pos;
            memcpy(dest, page_src, PAGE_SIZE);
            printf("Page copied to L2 cache at address %p\n", dest);
            l2_control.tag = reinterpret_cast<std::uintptr_t>(dest);
            l2_control.state = VALID;
            l2_control.dirty = CLEAN;
            // Move position forward
            l2_control.pos += PAGE_SIZE;
            if (l2_control.pos >= l2_control.size)
            {
                l2_control.pos = 0; // Wrap around to start
            }
        }

        void *l2_get_page(l2_control_data &l2_control, char *l2_data, std::size_t offset)
        {
            // Get page from L2 cache at given offset
            assert(l2_data != nullptr);
            assert(offset + PAGE_SIZE <= l2_control.size);

            void *src = static_cast<char *>(l2_data) + offset;
            return src;
        }

        void l2_extract(cxl_l2_info &l2, void *page_dest, std::size_t tag)
        {
            // Extract page from L2 cache to destination
            // Need to know where pages are stored, input is global address tag
            assert(page_dest != nullptr);
            assert(l2.base != nullptr);
            std::size_t offset = tag % l2.size; // Simple mapping for demonstration
            printf("Extracting page from L2 cache at offset %zu to destination %p\n", offset, page_dest);
            void *src = static_cast<char *>(l2.base) + offset;
            memcpy(page_dest, src, PAGE_SIZE);
        }

        bool l2_lookup(l2_control_data *controls, std::size_t entries, std::uintptr_t tag, std::size_t &index_out)
        {
            std::size_t idx = (tag / PAGE_SIZE) % entries;
            if (controls[idx].tag == tag && controls[idx].state == VALID)
            {
                index_out = idx;
                return true;
            }
            return false;
        }

    }
}