/*
 * Routing on the road graph (see route.h).
 */
#include <stdint.h>
#include <psxgte.h>
#include "route.h"
#include "world.h"

#define MAX_NODES   8192
#define HEAP_LEN    16384
#define INF         0x7fffffff
#define NONE        0xffff
#define AT_TARGET   0xfffe       /* next_edge: go along the target's own edge */
#define POPS_PER_FRAME 800
#define UTURN_M     60           /* turning round costs like this many metres */

RoadGraph graph;
uint32_t graph_data[GRAPH_BYTES / 4];

MapPoint route_points[ROUTE_POINTS];
volatile int32_t route_count, route_metres;

static int32_t dist[MAX_NODES];
static uint16_t next_edge[MAX_NODES];
/* key << 13 | node: keys (metres) up to 2^18, nodes up to MAX_NODES. */
static uint32_t heap[HEAP_LEN];
static int heap_len;

/* The target: on edge target_edge, target_t metres from its node a. */
static int has_target, tree_done;
static int target_edge, target_seg;
static int32_t target_t;
static MapPoint target_p;

static int frame;

int route_init(int bytes) {
	const uint16_t *h = (const uint16_t *)graph_data;
	const uint32_t *h32 = (const uint32_t *)graph_data;
	graph.nnodes = h[0];
	graph.nedges = h[1];
	graph.grid_w = h[2];
	graph.grid_h = h[3];
	graph.cell_m = h[4];
	graph.sea_w = h[5];
	graph.sea_h = h[6];
	graph.sea_cell = h[7];
	graph.sea_row_bytes = (graph.sea_w + 7) >> 3;
	int npoints = h32[4], nadj = h32[5], ncell = h32[6];
	if (graph.nnodes > MAX_NODES || !graph.cell_m || !graph.sea_cell) {
		graph.nnodes = graph.nedges = 0;
		return 0;
	}
	const uint8_t *p = (const uint8_t *)graph_data + 32;
	graph.nodes = (const MapPoint *)p;            p += graph.nnodes * 4;
	graph.adj_first = (const uint32_t *)p;        p += (graph.nnodes + 1) * 4;
	graph.adj = (const uint16_t *)p;              p += nadj * 2;
	p = (const uint8_t *)(((uintptr_t)p + 3) & ~3);
	graph.edges = (const RoadEdge *)p;            p += graph.nedges * sizeof(RoadEdge);
	graph.points = (const MapPoint *)p;           p += npoints * 4;
	graph.cell_first = (const uint32_t *)p;       p += (graph.grid_w * graph.grid_h + 1) * 4;
	graph.cell_edges = (const uint16_t *)p;       p += ncell * 2;
	p = (const uint8_t *)(((uintptr_t)p + 3) & ~3);
	graph.sea = p;                                p += graph.sea_h * graph.sea_row_bytes;
	if (p > (const uint8_t *)graph_data + bytes) {
		graph.nnodes = graph.nedges = 0;
		return 0;
	}
	return 1;
}

/* ---- Nearest edge -------------------------------------------------------------------- */

typedef struct {
	int edge, seg;      /* segment seg: between edge points seg and seg + 1 */
	int32_t t;          /* metres from node a along the edge */
	int32_t d2;         /* squared distance, metres^2 (+ heading penalty) */
	MapPoint p;         /* the nearest point */
	int along;          /* 1 if heading towards b, -1 towards a, 0 unknown */
} Snap;

static int32_t isqrt(int32_t v) {
	int32_t r = 0, bit = 1 << 30;
	while (bit > v) bit >>= 2;
	while (bit) {
		if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
		else r >>= 1;
		bit >>= 2;
	}
	return r;
}

/* The point nearest to (x, z) metres on the edges of the cells within
 * radius cells. With a heading (0..4095, or -1), edges across it count as
 * further away so the road the car drives along wins at a junction. */
static int snap(int32_t x, int32_t z, int radius, int heading, Snap *out) {
	int cx = x / graph.cell_m, cz = z / graph.cell_m;
	int32_t hx = 0, hz = 0;
	if (heading >= 0) { hx = isin(heading); hz = icos(heading); }
	out->edge = -1;
	out->d2 = INF;
	for (int gz = cz - radius; gz <= cz + radius; gz++) {
		if (gz < 0 || gz >= graph.grid_h) continue;
		for (int gx = cx - radius; gx <= cx + radius; gx++) {
			if (gx < 0 || gx >= graph.grid_w) continue;
			int c = gz * graph.grid_w + gx;
			for (uint32_t k = graph.cell_first[c]; k < graph.cell_first[c + 1]; k++) {
				const RoadEdge *e = &graph.edges[graph.cell_edges[k]];
				int32_t along = 0;
				for (int i = 0; i <= e->npoints; i++) {
					MapPoint a = edge_point(e, i), b = edge_point(e, i + 1);
					int32_t dx = b.x - a.x, dz = b.z - a.z;
					int32_t l2 = dx * dx + dz * dz;
					int32_t len = isqrt(l2);
					int32_t u = 0;   /* 0..4096 along the segment */
					if (l2 > 0) {
						int32_t num = (x - a.x) * dx + (z - a.z) * dz;
						u = l2 >= 4096 ? num * 64 / (l2 >> 6) : num * 4096 / l2;
						if (u < 0) u = 0;
						if (u > 4096) u = 4096;
					}
					int32_t px = a.x + (dx * u >> 12), pz = a.z + (dz * u >> 12);
					int32_t ex = x - px, ez = z - pz;
					int32_t d2 = ex * ex + ez * ez;
					int dir = 0;
					if (heading >= 0 && len > 0) {
						/* cos between the heading and the segment, 4096 = 1 */
						int32_t c = (hx * dx + hz * dz) / len;
						int32_t ac = c < 0 ? -c : c;
						d2 += (4096 - ac) * 100 >> 12;   /* across: like 10 m further */
						dir = c >= 0 ? 1 : -1;
					}
					if (d2 < out->d2) {
						out->d2 = d2;
						out->edge = e - graph.edges;
						out->seg = i;
						out->t = along + (len * u >> 12);
						out->p.x = px;
						out->p.z = pz;
						out->along = dir;
					}
					along += len;
				}
			}
		}
	}
	return out->edge >= 0;
}

/* ---- The tree ---------------------------------------------------------------------- */

static void push(int node, int32_t key) {
	if (heap_len >= HEAP_LEN || key >= 1 << 18) return;
	uint32_t v = (uint32_t)key << 13 | node;
	int i = heap_len++;
	while (i > 0) {
		int up = (i - 1) >> 1;
		if (heap[up] <= v) break;
		heap[i] = heap[up];
		i = up;
	}
	heap[i] = v;
}

static int pop(int32_t *key) {
	uint32_t top = heap[0], v = heap[--heap_len];
	int i = 0;
	for (;;) {
		int c = 2 * i + 1;
		if (c >= heap_len) break;
		if (c + 1 < heap_len && heap[c + 1] < heap[c]) c++;
		if (heap[c] >= v) break;
		heap[i] = heap[c];
		i = c;
	}
	heap[i] = v;
	*key = top >> 13;
	return top & 8191;
}

static int32_t cost(const RoadEdge *e) {
	return e->flags & ROAD_SERVICE ? e->length * 2 : e->length;
}

static void seed(int node, int32_t d) {
	if (d < dist[node]) {
		dist[node] = d;
		next_edge[node] = AT_TARGET;
		push(node, d);
	}
}

void route_clear(void) {
	has_target = 0;
	route_count = 0;
}

void route_set_target(int32_t x, int32_t z) {
	Snap s;
	route_clear();
	if (!graph.nedges || (!snap(x / UNIT, z / UNIT, 1, -1, &s) && !snap(x / UNIT, z / UNIT, 3, -1, &s)))
		return;
	has_target = 1;
	tree_done = 0;
	target_edge = s.edge;
	target_seg = s.seg;
	target_t = s.t;
	target_p = s.p;
	for (int i = 0; i < graph.nnodes; i++) {
		dist[i] = INF;
		next_edge[i] = NONE;
	}
	heap_len = 0;
	const RoadEdge *e = &graph.edges[s.edge];
	/* From a, along the edge to the target (always allowed); from b only
	 * if it is two-way. */
	seed(e->a, s.t);
	if (!(e->flags & ROAD_ONEWAY))
		seed(e->b, e->length - s.t > 0 ? e->length - s.t : 0);
}

/* Dijkstra backwards: dist[u] = metres from u to the target. */
static void grow(int pops) {
	while (pops-- > 0 && heap_len) {
		int32_t d;
		int v = pop(&d);
		if (d > dist[v]) continue;   /* stale */
		for (uint32_t k = graph.adj_first[v]; k < graph.adj_first[v + 1]; k++) {
			int ei = graph.adj[k];
			const RoadEdge *e = &graph.edges[ei];
			int u = e->a == v ? e->b : e->a;
			if (u == v) continue;
			if ((e->flags & ROAD_ONEWAY) && e->a != u) continue;   /* can't drive u -> v */
			int32_t nd = d + cost(e);
			if (nd < dist[u]) {
				dist[u] = nd;
				next_edge[u] = ei;
				push(u, nd);
			}
		}
	}
	if (!heap_len)
		tree_done = 1;
}

/* ---- Following it ------------------------------------------------------------------ */

static int add(MapPoint p) {
	if (route_count >= ROUTE_POINTS) return 0;
	if (route_count && route_points[route_count - 1].x == p.x && route_points[route_count - 1].z == p.z) return 1;
	route_points[route_count++] = p;
	return 1;
}

/* Edge e's points from node `from` to its other end (not including `from`). */
static int add_edge(const RoadEdge *e, int from) {
	if (from == e->a) {
		for (int i = 1; i <= e->npoints + 1; i++) if (!add(edge_point(e, i))) return 0;
	} else {
		for (int i = e->npoints; i >= 0; i--) if (!add(edge_point(e, i))) return 0;
	}
	return 1;
}

void route_update(int32_t car_x, int32_t car_z, int heading) {
	if (!has_target)
		return;
	if (!tree_done) {
		grow(POPS_PER_FRAME);
		if (!tree_done) return;
	}
	if (frame++ & 3)
		return;   /* the polyline every 4th frame */

	Snap s;
	int32_t x = car_x / UNIT, z = car_z / UNIT;
	if (!snap(x, z, 1, heading, &s)) {
		route_count = 0;
		return;
	}
	const RoadEdge *e = &graph.edges[s.edge];
	/* Options: on towards b, back towards a (two-way only), or straight to
	 * the target if it's on this edge; turning round costs UTURN_M. */
	int32_t to_b = dist[e->b] == INF ? INF : e->length - s.t + dist[e->b] + (s.along < 0 ? UTURN_M : 0);
	int32_t to_a = (e->flags & ROAD_ONEWAY) || dist[e->a] == INF ? INF : s.t + dist[e->a] + (s.along > 0 ? UTURN_M : 0);
	int32_t direct = INF;
	int direct_dir = 0;
	if (s.edge == target_edge) {
		if (target_t >= s.t) { direct = target_t - s.t + (s.along < 0 ? UTURN_M : 0); direct_dir = 1; }
		else if (!(e->flags & ROAD_ONEWAY)) { direct = s.t - target_t + (s.along > 0 ? UTURN_M : 0); direct_dir = -1; }
	}

	route_count = 0;
	add(s.p);
	int node;
	if (direct <= to_b && direct <= to_a && direct != INF) {
		if (direct_dir > 0) for (int i = s.seg + 1; i <= target_seg; i++) add(edge_point(e, i));
		else for (int i = s.seg; i > target_seg; i--) add(edge_point(e, i));
		add(target_p);
		route_metres = direct;
		return;
	} else if (to_b <= to_a && to_b != INF) {
		for (int i = s.seg + 1; i <= e->npoints + 1; i++) add(edge_point(e, i));
		node = e->b;
		route_metres = to_b;
	} else if (to_a != INF) {
		for (int i = s.seg; i >= 0; i--) add(edge_point(e, i));
		node = e->a;
		route_metres = to_a;
	} else {
		route_count = 0;   /* no way from here */
		return;
	}
	for (int steps = 0; steps < graph.nnodes; steps++) {
		int ei = next_edge[node];
		if (ei == AT_TARGET) {
			const RoadEdge *t = &graph.edges[target_edge];
			if (node == t->a) for (int i = 1; i <= target_seg; i++) add(edge_point(t, i));
			else for (int i = t->npoints; i > target_seg; i--) add(edge_point(t, i));
			add(target_p);
			return;
		}
		if (ei == NONE)
			break;
		const RoadEdge *n = &graph.edges[ei];
		if (!add_edge(n, node))
			return;   /* long route: the first ROUTE_POINTS points */
		node = n->a == node ? n->b : n->a;
	}
	route_count = 0;   /* broken tree: show nothing rather than nonsense */
}
