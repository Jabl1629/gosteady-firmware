/*
 * test_loader.h — load reference vector fixtures emitted by
 * algo/export_reference_vectors.py.
 *
 * The binary layout is documented in algo/export_reference_vectors.py
 * and mirrored here. Loader reads the whole file into a single
 * malloc'd buffer; pointers in the returned struct alias into that
 * buffer (do NOT free them individually — call test_fixture_free()).
 */

#ifndef GS_TEST_LOADER_H_
#define GS_TEST_LOADER_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TEST_FIXTURE_MAGIC      "GS_REFVT"
#define TEST_FIXTURE_VERSION    1u
#define TEST_FIXTURE_HEADER_BYTES   48u
#define TEST_FIXTURE_PEAK_BYTES     20u

struct test_peak {
	uint32_t sample_idx;
	float    time_s;
	float    amplitude_g;
	float    duration_s;
	float    energy_g2s;
};

struct test_fixture {
	void *_blob;                     /* owning pointer, free with test_fixture_free */
	size_t blob_bytes;

	/* Header fields */
	uint32_t version;
	uint32_t n_samples;
	uint32_t n_peaks;
	uint32_t surface_class;          /* 0=indoor, 1=outdoor */
	float    fs_hz;
	float    expected_R;             /* may be NaN */
	float    expected_distance_ft;
	float    expected_motion_duration_s;
	float    expected_motion_fraction;
	float    expected_total_duration_s;

	/* Per-sample arrays (alias into _blob; do not free) */
	const float   *mag_g;            /* length n_samples */
	const float   *mag_hp;           /* length n_samples */
	const float   *mag_lp;           /* length n_samples */
	const uint8_t *motion_mask;      /* length n_samples */

	/* Peaks (alias into _blob; do not free) */
	const struct test_peak *peaks;   /* length n_peaks; NULL if n_peaks=0 */
};

/* Load a fixture from disk. Returns 0 on success, -1 on error (msg
 * printed to stderr). */
int test_fixture_load(const char *path, struct test_fixture *fix);

/* Free fixture memory. Safe to call on a never-loaded fixture (no-op
 * if _blob is NULL). */
void test_fixture_free(struct test_fixture *fix);

#endif  /* GS_TEST_LOADER_H_ */
