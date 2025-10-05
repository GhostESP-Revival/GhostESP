#ifndef GC_TEST_H
#define GC_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

// Test function to validate garbage collection functionality
void gc_run_tests(void);

// Comprehensive verification tests
void gc_run_verification_tests(void);
void gc_run_leak_test(void);
void gc_run_stress_test(void);
void gc_run_benchmark(void);

#ifdef __cplusplus
}
#endif

#endif // GC_TEST_H
