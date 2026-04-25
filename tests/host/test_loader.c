/*
 * test_loader.c — see test_loader.h.
 */

#include "test_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int test_fixture_load(const char *path, struct test_fixture *fix)
{
	memset(fix, 0, sizeof(*fix));

	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		fprintf(stderr, "test_fixture_load: cannot open %s\n", path);
		return -1;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return -1;
	}
	const long sz = ftell(f);
	if (sz <= 0) {
		fclose(f);
		fprintf(stderr, "test_fixture_load: empty file %s\n", path);
		return -1;
	}
	rewind(f);

	void *blob = malloc((size_t)sz);
	if (blob == NULL) {
		fclose(f);
		return -1;
	}
	if (fread(blob, 1, (size_t)sz, f) != (size_t)sz) {
		fclose(f);
		free(blob);
		fprintf(stderr, "test_fixture_load: short read on %s\n", path);
		return -1;
	}
	fclose(f);

	fix->_blob = blob;
	fix->blob_bytes = (size_t)sz;

	const uint8_t *bytes = (const uint8_t *)blob;
	if ((size_t)sz < TEST_FIXTURE_HEADER_BYTES
	    || memcmp(bytes, TEST_FIXTURE_MAGIC, 8) != 0) {
		fprintf(stderr, "test_fixture_load: bad magic in %s\n", path);
		test_fixture_free(fix);
		return -1;
	}

	/* Header decode — keep in lock-step with
	 * algo/export_reference_vectors.py write_test_vector(). */
	memcpy(&fix->version,     bytes + 8,  4);
	memcpy(&fix->n_samples,   bytes + 12, 4);
	memcpy(&fix->n_peaks,     bytes + 16, 4);
	memcpy(&fix->surface_class, bytes + 20, 4);
	memcpy(&fix->fs_hz,       bytes + 24, 4);
	memcpy(&fix->expected_R,                 bytes + 28, 4);
	memcpy(&fix->expected_distance_ft,       bytes + 32, 4);
	memcpy(&fix->expected_motion_duration_s, bytes + 36, 4);
	memcpy(&fix->expected_motion_fraction,   bytes + 40, 4);
	memcpy(&fix->expected_total_duration_s,  bytes + 44, 4);

	if (fix->version != TEST_FIXTURE_VERSION) {
		fprintf(stderr, "test_fixture_load: %s version %u != expected %u\n",
		        path, fix->version, TEST_FIXTURE_VERSION);
		test_fixture_free(fix);
		return -1;
	}

	const uint32_t n = fix->n_samples;
	const size_t mag_g_off       = TEST_FIXTURE_HEADER_BYTES;
	const size_t mag_hp_off      = mag_g_off + 4u * n;
	const size_t mag_lp_off      = mag_g_off + 8u * n;
	const size_t motion_mask_off = mag_g_off + 12u * n;
	const size_t samples_block_end = motion_mask_off + n;
	const size_t pad = (4u - (samples_block_end & 3u)) & 3u;
	const size_t peaks_off = samples_block_end + pad;
	const size_t expected_total = peaks_off + (size_t)fix->n_peaks * TEST_FIXTURE_PEAK_BYTES;
	if (expected_total != (size_t)sz) {
		fprintf(stderr, "test_fixture_load: %s size mismatch — "
		        "computed %zu, file %zu\n",
		        path, expected_total, (size_t)sz);
		test_fixture_free(fix);
		return -1;
	}

	fix->mag_g       = (const float   *)(bytes + mag_g_off);
	fix->mag_hp      = (const float   *)(bytes + mag_hp_off);
	fix->mag_lp      = (const float   *)(bytes + mag_lp_off);
	fix->motion_mask = (const uint8_t *)(bytes + motion_mask_off);
	fix->peaks = (fix->n_peaks > 0u)
	             ? (const struct test_peak *)(bytes + peaks_off)
	             : NULL;
	return 0;
}

void test_fixture_free(struct test_fixture *fix)
{
	if (fix->_blob) {
		free(fix->_blob);
		fix->_blob = NULL;
	}
	fix->blob_bytes = 0;
	fix->mag_g = NULL;
	fix->mag_hp = NULL;
	fix->mag_lp = NULL;
	fix->motion_mask = NULL;
	fix->peaks = NULL;
}
