/*
 * Driving physics and the chase camera.
 *
 * Semi-arcade, tuned like an old W124 taxi: ~0-100 km/h in 9 s, about
 * 125 km/h flat out, firm brakes, the rear steps out a little at speed and
 * more with the handbrake. Steering follows the turning circle at low speed
 * and is limited by grip (about 0.8 g) at high speed.
 */
#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "car.h"
#include "car_model.h"

#define ACCEL        53        /* at standstill, per tick (~3.1 m/s^2) */
#define TOP_SPEED    37000     /* ~133 km/h, where the engine gives no more */
#define BRAKE        150       /* ~8.8 m/s^2 */
#define REVERSE_MAX  4600      /* ~16 km/h */
#define MIN_TURN_R   88        /* turning radius in 1/16 m (5.5 m) */
#define BODY_R       (230)     /* collision circle radius, 1/256 m (0.9 m) */
#define BODY_OFF     (85 * CAR_M / 64) /* circle centres +-1.33 m from the middle */

void car_reset(Car *car, int32_t world_x, int32_t world_z, int heading) {
	car->x = world_x << 12;
	car->z = world_z << 12;
	car->vx = car->vz = 0;
	car->heading = heading << 8;
	car->steer = 0;
	car->speed = 0;
	car->bumps = 0;
}

int car_kmh(const Car *car) {
	/* m/tick * 60 * 3.6 */
	int s = car->speed < 0 ? -car->speed : car->speed;
	return s * 216 / CAR_M;
}

static void collide(Car *car) {
	int h = car->heading >> 8;
	int32_t fx = isin(h), fz = icos(h);
	int32_t off_x = (int32_t)((int64_t)BODY_OFF * fx >> 12), off_z = (int32_t)((int64_t)BODY_OFF * fz >> 12);
	for (int end = -1; end <= 1; end += 2) {
		/* 1/256 m for the world. */
		int32_t cx = (car->x + end * off_x) >> 8, cz = (car->z + end * off_z) >> 8;
		int32_t ox = cx, oz = cz, nx, nz;
		if (!world_collide(&cx, &cz, BODY_R, &nx, &nz))
			continue;
		car->x += (cx - ox) << 8;
		car->z += (cz - oz) << 8;
		/* Take away the speed into the wall, and some scraping. */
		int32_t vn = (car->vx * nx + car->vz * nz) >> 12;
		if (vn < 0) {
			car->vx -= vn * nx >> 12;
			car->vz -= vn * nz >> 12;
			car->vx -= car->vx >> 4;
			car->vz -= car->vz >> 4;
			if (vn < -CAR_M / 60)   /* harder than ~1 m/s */
				car->bumps++;
		}
	}
}

void car_tick(Car *car, const CarInput *in) {
	/* Steering wheel: the D-pad turns it gradually, a stick sets it. */
	if (in->analog) car->steer = in->steer;
	else if (car->steer < in->steer) car->steer = car->steer + 600 > in->steer ? in->steer : car->steer + 600;
	else if (car->steer > in->steer) car->steer = car->steer - 600 < in->steer ? in->steer : car->steer - 600;

	/* Yaw from the forward speed and the turning radius: the radius grows
	 * with speed so sideways grip stays near 0.8 g. */
	int32_t s = car->speed;
	int32_t v16 = (s < 0 ? -s : s) * 15 >> 10;       /* speed in 1/16 m/s */
	int32_t r = v16 * v16 / 128;
	if (r < MIN_TURN_R) r = MIN_TURN_R;
	/* heading change (4096/turn << 8) = v / R in radians * 652 * 256 */
	int32_t dh = (int32_t)((int64_t)s * 41 * car->steer / 4096 / r);
	car->heading = (car->heading + dh) & ((4096 << 8) - 1);

	int h = car->heading >> 8;
	int32_t fx = isin(h), fz = icos(h);   /* forward */
	int32_t rx = fz, rz = -fx;            /* right */
	int32_t vlong = (car->vx * fx + car->vz * fz) >> 12;
	int32_t vlat = (car->vx * rx + car->vz * rz) >> 12;

	if (in->gas) {
		if (vlong < -200) vlong += BRAKE;   /* gas while rolling back = brake */
		else vlong += ACCEL * (TOP_SPEED - vlong) / TOP_SPEED;
	}
	if (in->brake) {
		if (vlong > 200) vlong -= BRAKE;
		else if (vlong > -REVERSE_MAX) vlong -= ACCEL / 2;   /* reverse */
	}
	if (!in->gas && !in->brake) {
		/* Engine braking and rolling resistance. */
		if (vlong > 12) vlong -= 12;
		else if (vlong < -12) vlong += 12;
		else vlong = 0;
	}
	vlong -= vlong >> 10;                 /* air drag */
	if (in->handbrake) {
		vlong -= vlong >> 6;
		vlat -= vlat >> 5;                /* rear lets go: drift */
	} else if (v16 > 400) {
		vlat -= vlat >> 3;                /* fast: a little slide */
	} else {
		vlat -= vlat >> 1;
	}

	car->speed = vlong;
	car->vx = (vlong * fx + vlat * rx) >> 12;
	car->vz = (vlong * fz + vlat * rz) >> 12;
	car->x += car->vx;
	car->z += car->vz;

	/* Stay inside the map. */
	int32_t w, d;
	world_size(&w, &d);
	if (car->x < 2 * CAR_M) car->x = 2 * CAR_M, car->vx = 0;
	if (car->z < 2 * CAR_M) car->z = 2 * CAR_M, car->vz = 0;
	if (car->x > (w << 12) - 2 * CAR_M) car->x = (w << 12) - 2 * CAR_M, car->vx = 0;
	if (car->z > (d << 12) - 2 * CAR_M) car->z = (d << 12) - 2 * CAR_M, car->vz = 0;

	collide(car);
	car->speed = (car->vx * fx + car->vz * fz) >> 12;
}

/* ---- Camera ---------------------------------------------------------------------- */

#define CAM_DIST    (65 * UNIT / 10)   /* 6.5 m behind */
#define CAM_HEIGHT  (24 * UNIT / 10)   /* 2.4 m up */
#define CAM_PITCH   110                /* ~9.7 degrees down */

void car_camera(const Car *car, Camera *cam, int snap) {
	int target = car->heading >> 8;
	/* Reversing: keep looking forward. */
	if (snap) cam->heading = target;
	else {
		int d = ((target - cam->heading + 2048) & 4095) - 2048;
		cam->heading = (cam->heading + d / 6) & 4095;
	}
	cam->pitch = CAM_PITCH;
	int32_t cx = CAR_TO_WORLD(car->x), cz = CAR_TO_WORLD(car->z);
	cam->x = cx - (isin(cam->heading) * CAM_DIST >> 12);
	cam->z = cz - (icos(cam->heading) * CAM_DIST >> 12);
	cam->y = -CAM_HEIGHT;
	camera_update(cam);
}

/* ---- Drawing --------------------------------------------------------------------- */

uint8_t *car_draw(const Car *car, const Camera *cam, uint32_t *ot, uint8_t *prims) {
	int h = car->heading >> 8;
	int s = isin(h), c = icos(h);
	/* car space -> world: columns are the car's right, down and forward. */
	MATRIX local = { { { c, 0, s }, { 0, 4096, 0 }, { -s, 0, c } }, { 0, 0, 0 } };
	MATRIX m;
	MulMatrix0((MATRIX *)&cam->gm, &local, &m);
	int32_t dx = CAR_TO_WORLD(car->x) - cam->x, dy = -cam->y, dz = CAR_TO_WORLD(car->z) - cam->z;
	m.t[0] = (cam->m.m[0][0] * dx + cam->m.m[0][1] * dy + cam->m.m[0][2] * dz) * GTE_SCALE >> 12;
	m.t[1] = (cam->m.m[1][0] * dx + cam->m.m[1][1] * dy + cam->m.m[1][2] * dz) * GTE_SCALE >> 12;
	m.t[2] = (cam->m.m[2][0] * dx + cam->m.m[2][1] * dy + cam->m.m[2][2] * dz) * GTE_SCALE >> 12;
	gte_load_matrix(&m);

	static DVECTOR p[CAR_NVERTS + 2];
	static int32_t z[CAR_NVERTS + 2];
	int i = 0;
	for (; i + 3 <= CAR_NVERTS; i += 3) {
		gte_ldv3(&car_verts[i], &car_verts[i + 1], &car_verts[i + 2]);
		gte_rtpt();
		gte_stsxy3(&p[i], &p[i + 1], &p[i + 2]);
		gte_stsz3(&z[i], &z[i + 1], &z[i + 2]);
	}
	for (; i < CAR_NVERTS; i++) {
		gte_ldv0(&car_verts[i]);
		gte_rtps();
		gte_stsxy(&p[i]);
		gte_stsz(&z[i]);
	}

	for (int k = 0; k < CAR_NFACES; k++) {
		const uint8_t *f = car_faces[k];
		DVECTOR a = p[f[0]], b = p[f[1]], cc = p[f[2]], d = p[f[3]];
		if (z[f[0]] < NEAR_Z || z[f[1]] < NEAR_Z || z[f[2]] < NEAR_Z || z[f[3]] < NEAR_Z)
			continue;
		if ((b.vx - a.vx) * (cc.vy - a.vy) - (b.vy - a.vy) * (cc.vx - a.vx) <= 0)
			continue;
		int otz = (z[f[0]] + z[f[1]] + z[f[2]] + z[f[3]]) >> 6;
		if (otz < 1) otz = 1;
		if (otz > OT_LEN - 8) otz = OT_LEN - 8;
		if (f[4]) {
			POLY_F3 *q = (POLY_F3 *)prims;
			setPolyF3(q);
			setXY3(q, a.vx, a.vy, b.vx, b.vy, cc.vx, cc.vy);
			setRGB0(q, f[5], f[6], f[7]);
			addPrim(ot + otz, q);
			prims += sizeof(POLY_F3);
		} else {
			POLY_F4 *q = (POLY_F4 *)prims;
			setPolyF4(q);
			setXY4(q, a.vx, a.vy, b.vx, b.vy, cc.vx, cc.vy, d.vx, d.vy);
			setRGB0(q, f[5], f[6], f[7]);
			addPrim(ot + otz, q);
			prims += sizeof(POLY_F4);
		}
	}
	return prims;
}
