/*
 * The road graph from MAP.BIN (tools/osm2map.mjs, "Road graph"): the route
 * from the taxi to the passenger or destination along real streets,
 * respecting one-way streets, and what the big map draws.
 *
 * The route is a shortest-path tree grown backwards from the target over
 * a few frames (Dijkstra), so wherever the taxi goes, the way on from there
 * is already known: taking another turn just shows the new way at once.
 */
#pragma once
#include <stdint.h>

typedef struct {
	uint16_t a, b, length;   /* nodes, metres */
	uint8_t npoints, flags;  /* ROAD_* */
	uint32_t first_point;
} RoadEdge;
#define ROAD_CLASS   3       /* 0 minor, 1 secondary/tertiary, 2 primary+ */
#define ROAD_ONEWAY  4       /* only a -> b */
#define ROAD_SERVICE 8

typedef struct {
	int16_t x, z;            /* metres */
} MapPoint;

typedef struct {
	int nnodes, nedges, grid_w, grid_h, cell_m, sea_w, sea_h, sea_cell, sea_row_bytes;
	const MapPoint *nodes, *points;
	const uint32_t *adj_first;
	const uint16_t *adj;
	const RoadEdge *edges;
	const uint32_t *cell_first;
	const uint16_t *cell_edges;
	const uint8_t *sea;
} RoadGraph;

extern RoadGraph graph;

#define GRAPH_BYTES (192 * 1024)   /* must match MAX_GRAPH_BYTES in tools/osm2map.mjs */
extern uint32_t graph_data[GRAPH_BYTES / 4];

/* After world_init has read the graph into graph_data; 0 if it is bad. */
int route_init(int bytes);

/* Route to (x, z) in world units, or none with route_clear(). */
void route_set_target(int32_t x, int32_t z);
void route_clear(void);
/* Once a frame: grows the tree a bit and follows it from the car. */
void route_update(int32_t car_x, int32_t car_z, int heading);

/* The route from the car to the target, metres, when known. */
#define ROUTE_POINTS 1024
extern MapPoint route_points[ROUTE_POINTS];
extern volatile int32_t route_count;      /* 0 = no route (yet) */
extern volatile int32_t route_metres;     /* its length */

/* Edge e's i-th point (0 = node a, npoints + 1 = node b). */
static inline MapPoint edge_point(const RoadEdge *e, int i) {
	if (i == 0) return graph.nodes[e->a];
	if (i > e->npoints) return graph.nodes[e->b];
	return graph.points[e->first_point + i - 1];
}
