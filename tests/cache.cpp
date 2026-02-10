
// C headers
#include <mpi.h>
// C++ headers
#include <algorithm>
#include <chrono>
#include <numeric>
#include <random>
// ArgoDSM headers
#include "argo.hpp"
#include "data_distribution/global_ptr.hpp"
// GoogleTest headers
#include "gtest/gtest.h"

/** @brief Global pointer to char */
using global_char = typename argo::data_distribution::global_ptr<char>;
/** @brief Global pointer to double */
using global_double = typename argo::data_distribution::global_ptr<double>;
/** @brief Global pointer to int */
using global_int = typename argo::data_distribution::global_ptr<int>;
/** @brief Global pointer to unsigned int */
using global_uint = typename argo::data_distribution::global_ptr<unsigned>;
/** @brief Global pointer to int pointer */
using global_intptr = typename argo::data_distribution::global_ptr<int*>;

/** @brief ArgoDSM memory size (16M) */
constexpr std::size_t size = 1<<24;
/** @brief ArgoDSM cache size */
constexpr std::size_t cache_size = size/2;
/** @brief ArgoDSM array size */
constexpr std::size_t array_size = 1<<19;

/** @brief Time to wait before assuming a deadlock has occurred */
constexpr std::chrono::minutes deadlock_threshold{1};  // Chosen for no reason

/** @brief A random char constant */
constexpr char c_const = 'a';
/** @brief A random int constant */
constexpr int i_const = 42;
/** @brief A large random int constant */
constexpr unsigned j_const = 2124481224;
/** @brief A random double constant */
constexpr double d_const = 1.0/3.0 * 3.14159;

/**
 * @brief Class for the gtests fixture tests. Will reset the allocators to a clean state for every test
 */
class cacheTest : public testing::Test, public ::testing::WithParamInterface<int> {
	protected:
		cacheTest()  {
			argo::reset();
		}

		~cacheTest() {
			argo::barrier();
		}
};



/**
 * @brief Test that L2 cache invalidates entries when L1 cache invalidates pages
 * 
 * This test verifies that when a page is invalidated in the L1 cache due to
 * coherence operations (e.g., during self-invalidation with multiple writers),
 * the corresponding entry in the L2 cache is also properly invalidated.
 */
TEST_F(cacheTest, l2InvalidationOnL1Invalidation) {
    // Main memory 16MB, L1 8MB and L2 16 MB (Right now L2 is always double the L1)
    // Fill L1 first, then access more pages to push some to L2
    const std::size_t page_size = 4096;
    const std::size_t l1_pages = (8 * 1024 * 1024) / page_size; 
    const std::size_t extra_pages = 1024;  // Extra pages to force L2 usage
    const std::size_t total_pages = l1_pages + extra_pages;
    const std::size_t ints_per_page = page_size / sizeof(int);
    const std::size_t total_ints = total_pages * ints_per_page;
    
    int* array = argo::conew_array<int>(total_ints);
    
    // Initialize array on node 0
    if(argo::node_id() == 0) {
        for(std::size_t i = 0; i < total_ints; i++) {
            array[i] = i_const;
        }
    }
    argo::barrier();
    
    // All nodes read the entire array to fill L1 cache
    // The extra pages beyond L1 capacity will cause evictions to L2
    volatile int sum = 0;
    for(std::size_t i = 0; i < total_ints; i++) {
        sum += array[i];
    }
    
    // Now access the beginning of the array again
    // This should bring pages back from L2 to L1
    sum = 0;
    for(std::size_t i = 0; i < ints_per_page * extra_pages; i++) {
        sum += array[i];
    }
    
    // All nodes write to create multiple writers
    // Focus on the pages that were likely in L2
    for(std::size_t i = 0; i < ints_per_page * extra_pages; i++) {
        array[i] = i_const + argo::node_id();
    }
    
    // Barrier triggers self-invalidation for pages with multiple writers
    // Both L1 and L2 entries should be invalidated
    argo::barrier();
    
    // Access more pages to potentially push our test pages to L2 again
    sum = 0;
    for(std::size_t i = total_ints - (ints_per_page * extra_pages); i < total_ints; i++) {
        sum += array[i];
    }
    
    // Node 0 writes new values to the test pages
    if(argo::node_id() == 0) {
        for(std::size_t i = 0; i < ints_per_page * extra_pages; i++) {
            array[i] = j_const;
        }
    }
    argo::barrier();
    
    // All nodes read again if L2 wasn't invalidated properly,
    // stale data from L2 might be loaded instead of fresh data
    sum = 0;
    for(std::size_t i = 0; i < ints_per_page * extra_pages; i++) {
        sum += array[i];
    }
    ASSERT_EQ(sum, static_cast<int>(j_const * ints_per_page * extra_pages));
    
    argo::codelete_array(array);
}


/**
 * @brief The main function that runs the tests
 * @param argc Number of command line arguments
 * @param argv Command line arguments
 * @return 0 if success
 */
int main(int argc, char **argv) {
	argo::init(size, cache_size);
	::testing::InitGoogleTest(&argc, argv);
	auto res = RUN_ALL_TESTS();
	argo::finalize();
	return res;
}
