/*
 * test_rollator_distance.c — host driver for gs_rollator_distance parity test.
 * Reads a v1 .dat session (256-byte header + 28-byte samples: t_ms + 6 float32
 * ax,ay,az,gx,gy,gz), feeds mag_g = |accel|/9.80665 through the C estimator, and
 * prints "valid distance_ft flatness vib_int walk_t_s n_active". A companion
 * Python (rollator_distance_ref.compute) is run on the same files and compared.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "gs_rollator_distance.h"

#define HDR_BYTES 256
#define SMP_BYTES 28

static struct gs_roll_distance d;

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s file.dat\n", argv[0]); return 2; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) { fprintf(stderr, "open failed\n"); return 2; }
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, HDR_BYTES, SEEK_SET);
	long nsmp = (sz - HDR_BYTES) / SMP_BYTES;

	gs_roll_distance_init(&d);
	unsigned char buf[SMP_BYTES];
	for (long i = 0; i < nsmp; i++) {
		if (fread(buf, 1, SMP_BYTES, f) != SMP_BYTES) break;
		float ax, ay, az;
		memcpy(&ax, buf + 4, 4);
		memcpy(&ay, buf + 8, 4);
		memcpy(&az, buf + 12, 4);
		float mag_g = sqrtf(ax*ax + ay*ay + az*az) / 9.80665f;
		gs_roll_distance_step(&d, mag_g);
	}
	fclose(f);

	struct gs_roll_outputs o;
	gs_roll_distance_finalize(&d, &o);
	printf("%d %.4f %.6f %.4f %.2f %u\n",
	       o.valid ? 1 : 0, o.distance_ft, o.flatness, o.vib_int,
	       o.walk_t_s, o.n_active_windows);
	return 0;
}
