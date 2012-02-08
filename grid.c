/*
 * (c) Lambros Lambrou 2008
 *
 * Code for working with general grids, which can be any planar graph
 * with faces, edges and vertices (dots).  Includes generators for a few
 * types of grid, including square, hexagonal, triangular and others.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <float.h>

#include "puzzles.h"
#include "tree234.h"
#include "grid.h"
#include "penrose.h"

/* Debugging options */

/*
#define DEBUG_GRID
*/

/* ----------------------------------------------------------------------
 * Deallocate or dereference a grid
 */
void grid_free(grid *g)
{
    assert(g->refcount);

    g->refcount--;
    if (g->refcount == 0) {
        int i;
        for (i = 0; i < g->num_faces; i++) {
            sfree(g->faces[i].dots);
            sfree(g->faces[i].edges);
        }
        for (i = 0; i < g->num_dots; i++) {
            sfree(g->dots[i].faces);
            sfree(g->dots[i].edges);
        }
        sfree(g->faces);
        sfree(g->edges);
        sfree(g->dots);
        sfree(g);
    }
}

/* Used by the other grid generators.  Create a brand new grid with nothing
 * initialised (all lists are NULL) */
static grid *grid_empty(void)
{
    grid *g = snew(grid);
    g->faces = NULL;
    g->edges = NULL;
    g->dots = NULL;
    g->num_faces = g->num_edges = g->num_dots = 0;
    g->refcount = 1;
    g->lowest_x = g->lowest_y = g->highest_x = g->highest_y = 0;
    return g;
}

/* Helper function to calculate perpendicular distance from
 * a point P to a line AB.  A and B mustn't be equal here.
 *
 * Well-known formula for area A of a triangle:
 *                             /  1   1   1 \
 * 2A = determinant of matrix  | px  ax  bx |
 *                             \ py  ay  by /
 *
 * Also well-known: 2A = base * height
 *                     = perpendicular distance * line-length.
 *
 * Combining gives: distance = determinant / line-length(a,b)
 */
static double point_line_distance(long px, long py,
                                  long ax, long ay,
                                  long bx, long by)
{
    long det = ax*by - bx*ay + bx*py - px*by + px*ay - ax*py;
    double len;
    det = max(det, -det);
    len = sqrt(SQ(ax - bx) + SQ(ay - by));
    return det / len;
}

/* Determine whether points a, b, c are oriented
 * clockwise or anticlockwise. Returns positive
 * if yes, negative if no, and zero if they're collinear
 */
static long anticlockwise(long ax, long ay,
                      long bx, long by,
                      long cx, long cy)
{
    return bx*cy - cx*by + cx*ay - ax*cy + ax*by - bx*ay;
}

/* Determine whether d is in the triangle a, b, c
 * Assumes a, b, c is oriented anticlockwise
 */
static int in_triangle(long ax, long ay,
                       long bx, long by,
                       long cx, long cy,
                       long dx, long dy) 
{
    assert(anticlockwise(ax,ay,bx,by,cx,cy));
    return anticlockwise(ax,ay,bx,by,dx,dy)>0 &&
           anticlockwise(bx,by,cx,cy,dx,dy)>0 &&
           anticlockwise(cx,cy,ax,ay,dx,dy)>0;
}

/* Determine whether d is in the circumcircle of points a, b, and c
 * Requires a, b, and c to be in counterclockwise order
 * Returns a positive number iff the result is true.
 *
 * Warning: keep all coordinates smaller than the cube root
 * of MAXINT (fourth root?)
 */
static int in_circumcircle(long ax, long ay,
                            long bx, long by,
                            long cx, long cy,
                            long dx, long dy) 
{
    long a[3][3];
    a[0][0] = ax-dx;
    a[0][1] = ay-dy; 
    a[0][2] = (ax-dx)*(ax-dx)+(ay-dy)*(ay-dy);

    a[1][0] = bx-dx;
    a[1][1] = by-dy; 
    a[1][2] = (bx-dx)*(bx-dx)+(by-dy)*(by-dy);

    a[2][0] = cx-dx;
    a[2][1] = cy-dy; 
    a[2][2] = (cx-dx)*(cx-dx)+(cy-dy)*(cy-dy);

    return (a[0][0]*(a[1][1]*a[2][2]-a[2][1]*a[1][2])
            -a[1][0]*(a[0][1]*a[2][2]-a[2][1]*a[0][2])
            +a[2][0]*(a[0][1]*a[1][2]-a[1][1]*a[0][2]))>0;
}

/* Use subrandom sequences for point poisitions */
static double subrandom(long base, long n)
{
    double r=0;
    double b=1;
    while (n>0) 
    {
        b /= base;
        r += (n%base) * b;
        n /= base;
    }
    return r;
}

/* Determine nearest edge to where the user clicked.
 * (x, y) is the clicked location, converted to grid coordinates.
 * Returns the nearest edge, or NULL if no edge is reasonably
 * near the position.
 *
 * Just judging edges by perpendicular distance is not quite right -
 * the edge might be "off to one side". So we insist that the triangle
 * with (x,y) has acute angles at the edge's dots.
 *
 *     edge1
 *  *---------*------
 *            |
 *            |      *(x,y)
 *      edge2 |
 *            |   edge2 is OK, but edge1 is not, even though
 *            |   edge1 is perpendicularly closer to (x,y)
 *            *
 *
 */
grid_edge *grid_nearest_edge(grid *g, int x, int y)
{
    grid_edge *best_edge;
    double best_distance = 0;
    int i;

    best_edge = NULL;

    for (i = 0; i < g->num_edges; i++) {
        grid_edge *e = &g->edges[i];
        long e2; /* squared length of edge */
        long a2, b2; /* squared lengths of other sides */
        double dist;

        /* See if edge e is eligible - the triangle must have acute angles
         * at the edge's dots.
         * Pythagoras formula h^2 = a^2 + b^2 detects right-angles,
         * so detect acute angles by testing for h^2 < a^2 + b^2 */
        e2 = SQ((long)e->dot1->x - (long)e->dot2->x) + SQ((long)e->dot1->y - (long)e->dot2->y);
        a2 = SQ((long)e->dot1->x - (long)x) + SQ((long)e->dot1->y - (long)y);
        b2 = SQ((long)e->dot2->x - (long)x) + SQ((long)e->dot2->y - (long)y);
        if (a2 >= e2 + b2) continue;
        if (b2 >= e2 + a2) continue;
         
        /* e is eligible so far.  Now check the edge is reasonably close
         * to where the user clicked.  Don't want to toggle an edge if the
         * click was way off the grid.
         * There is room for experimentation here.  We could check the
         * perpendicular distance is within a certain fraction of the length
         * of the edge.  That amounts to testing a rectangular region around
         * the edge.
         * Alternatively, we could check that the angle at the point is obtuse.
         * That would amount to testing a circular region with the edge as
         * diameter. */
        dist = point_line_distance((long)x, (long)y,
                                   (long)e->dot1->x, (long)e->dot1->y,
                                   (long)e->dot2->x, (long)e->dot2->y);
        /* Is dist more than half edge length ? */
        if (4 * SQ(dist) > e2)
            continue;

        if (best_edge == NULL || dist < best_distance) {
            best_edge = e;
            best_distance = dist;
        }
    }
    return best_edge;
}

/* ----------------------------------------------------------------------
 * Grid generation
 */

#ifdef SVG_GRID

#define SVG_DOTS  1
#define SVG_EDGES 2
#define SVG_FACES 4

#define FACE_COLOUR "red"
#define EDGE_COLOUR "blue"
#define DOT_COLOUR "black"

static void grid_output_svg(FILE *fp, grid *g, int which)
{
    int i, j;

    fprintf(fp,"\
<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n\
<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 20010904//EN\"\n\
\"http://www.w3.org/TR/2001/REC-SVG-20010904/DTD/svg10.dtd\">\n\
\n\
<svg xmlns=\"http://www.w3.org/2000/svg\"\n\
xmlns:xlink=\"http://www.w3.org/1999/xlink\">\n\n");

    if (which & SVG_FACES) {
        fprintf(fp, "<g>\n");
        for (i = 0; i < g->num_faces; i++) {
            grid_face *f = g->faces + i;
            fprintf(fp, "<polygon points=\"");
            for (j = 0; j < f->order; j++) {
                grid_dot *d = f->dots[j];
                fprintf(fp, "%s%d,%d", (j == 0) ? "" : " ",
                        d->x, d->y);
            }
            fprintf(fp, "\" style=\"fill: %s; fill-opacity: 0.2; stroke: %s\" />\n",
                    FACE_COLOUR, FACE_COLOUR);
        }
        fprintf(fp, "</g>\n");
    }
    if (which & SVG_EDGES) {
        fprintf(fp, "<g>\n");
        for (i = 0; i < g->num_edges; i++) {
            grid_edge *e = g->edges + i;
            grid_dot *d1 = e->dot1, *d2 = e->dot2;

            fprintf(fp, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" "
                        "style=\"stroke: %s\" />\n",
                        d1->x, d1->y, d2->x, d2->y, EDGE_COLOUR);
        }
        fprintf(fp, "</g>\n");
    }

    if (which & SVG_DOTS) {
        fprintf(fp, "<g>\n");
        for (i = 0; i < g->num_dots; i++) {
            grid_dot *d = g->dots + i;
            fprintf(fp, "<ellipse cx=\"%d\" cy=\"%d\" rx=\"%d\" ry=\"%d\" fill=\"%s\" />",
                    d->x, d->y, g->tilesize/20, g->tilesize/20, DOT_COLOUR);
        }
        fprintf(fp, "</g>\n");
    }

    fprintf(fp, "</svg>\n");
}
#endif

#ifdef SVG_GRID
#include <errno.h>

static void grid_try_svg(grid *g, int which)
{
    char *svg = getenv("PUZZLES_SVG_GRID");
    if (svg) {
        FILE *svgf = fopen(svg, "w");
        if (svgf) {
            grid_output_svg(svgf, g, which);
            fclose(svgf);
        } else {
            fprintf(stderr, "Unable to open file `%s': %s", svg, strerror(errno));
        }
    }
}
#endif

/* Show the basic grid information, before doing grid_make_consistent */
static void grid_debug_basic(grid *g)
{
    /* TODO: Maybe we should generate an SVG image of the dots and lines
     * of the grid here, before grid_make_consistent.
     * Would help with debugging grid generation. */
#ifdef DEBUG_GRID
    int i;
    printf("--- Basic Grid Data ---\n");
    for (i = 0; i < g->num_faces; i++) {
        grid_face *f = g->faces + i;
        printf("Face %d: dots[", i);
        int j;
        for (j = 0; j < f->order; j++) {
            grid_dot *d = f->dots[j];
            printf("%s%d", j ? "," : "", (int)(d - g->dots)); 
        }
        printf("]\n");
    }
#endif
#ifdef SVG_GRID
    grid_try_svg(g, SVG_FACES);
#endif
}

/* Show the derived grid information, computed by grid_make_consistent */
static void grid_debug_derived(grid *g)
{
#ifdef DEBUG_GRID
    /* edges */
    int i;
    printf("--- Derived Grid Data ---\n");
    for (i = 0; i < g->num_edges; i++) {
        grid_edge *e = g->edges + i;
        printf("Edge %d: dots[%d,%d] faces[%d,%d]\n",
            i, (int)(e->dot1 - g->dots), (int)(e->dot2 - g->dots),
            e->face1 ? (int)(e->face1 - g->faces) : -1,
            e->face2 ? (int)(e->face2 - g->faces) : -1);
    }
    /* faces */
    for (i = 0; i < g->num_faces; i++) {
        grid_face *f = g->faces + i;
        int j;
        printf("Face %d: faces[", i);
        for (j = 0; j < f->order; j++) {
            grid_edge *e = f->edges[j];
            grid_face *f2 = (e->face1 == f) ? e->face2 : e->face1;
            printf("%s%d", j ? "," : "", f2 ? (int)(f2 - g->faces) : -1);
        }
        printf("]\n");
    }
    /* dots */
    for (i = 0; i < g->num_dots; i++) {
        grid_dot *d = g->dots + i;
        int j;
        printf("Dot %d: dots[", i);
        for (j = 0; j < d->order; j++) {
            grid_edge *e = d->edges[j];
            grid_dot *d2 = (e->dot1 == d) ? e->dot2 : e->dot1;
            printf("%s%d", j ? "," : "", (int)(d2 - g->dots));
        }
        printf("] faces[");
        for (j = 0; j < d->order; j++) {
            grid_face *f = d->faces[j];
            printf("%s%d", j ? "," : "", f ? (int)(f - g->faces) : -1);
        }
        printf("]\n");
    }
#endif
#ifdef SVG_GRID
    grid_try_svg(g, SVG_DOTS | SVG_EDGES | SVG_FACES);
#endif
}

/* Helper function for building incomplete-edges list in
 * grid_make_consistent() */
static int grid_edge_bydots_cmpfn(void *v1, void *v2)
{
    grid_edge *a = v1;
    grid_edge *b = v2;
    grid_dot *da, *db;

    /* Pointer subtraction is valid here, because all dots point into the
     * same dot-list (g->dots).
     * Edges are not "normalised" - the 2 dots could be stored in any order,
     * so we need to take this into account when comparing edges. */

    /* Compare first dots */
    da = (a->dot1 < a->dot2) ? a->dot1 : a->dot2;
    db = (b->dot1 < b->dot2) ? b->dot1 : b->dot2;
    if (da != db)
        return db - da;
    /* Compare last dots */
    da = (a->dot1 < a->dot2) ? a->dot2 : a->dot1;
    db = (b->dot1 < b->dot2) ? b->dot2 : b->dot1;
    if (da != db)
        return db - da;

    return 0;
}

/*
 * 'Vigorously trim' a grid, by which I mean deleting any isolated or
 * uninteresting faces. By which, in turn, I mean: ensure that the
 * grid is composed solely of faces adjacent to at least one
 * 'landlocked' dot (i.e. one not in contact with the infinite
 * exterior face), and that all those dots are in a single connected
 * component.
 *
 * This function operates on, and returns, a grid satisfying the
 * preconditions to grid_make_consistent() rather than the
 * postconditions. (So call it first.)
 */
static void grid_trim_vigorously(grid *g)
{
    int *dotpairs, *faces, *dots;
    int *dsf;
    int i, j, k, size, newfaces, newdots;

    /*
     * First construct a matrix in which each ordered pair of dots is
     * mapped to the index of the face in which those dots occur in
     * that order.
     */
    dotpairs = snewn(g->num_dots * g->num_dots, int);
    for (i = 0; i < g->num_dots; i++)
        for (j = 0; j < g->num_dots; j++)
            dotpairs[i*g->num_dots+j] = -1;
    for (i = 0; i < g->num_faces; i++) {
        grid_face *f = g->faces + i;
        int dot0 = f->dots[f->order-1] - g->dots;
        for (j = 0; j < f->order; j++) {
            int dot1 = f->dots[j] - g->dots;
            dotpairs[dot0 * g->num_dots + dot1] = i;
            dot0 = dot1;
        }
    }

    /*
     * Now we can identify landlocked dots: they're the ones all of
     * whose edges have a mirror-image counterpart in this matrix.
     */
    dots = snewn(g->num_dots, int);
    for (i = 0; i < g->num_dots; i++) {
        dots[i] = TRUE;
        for (j = 0; j < g->num_dots; j++) {
            if ((dotpairs[i*g->num_dots+j] >= 0) ^
                (dotpairs[j*g->num_dots+i] >= 0))
                dots[i] = FALSE;    /* non-duplicated edge: coastal dot */
        }
    }

    /*
     * Now identify connected pairs of landlocked dots, and form a dsf
     * unifying them.
     */
    dsf = snew_dsf(g->num_dots);
    for (i = 0; i < g->num_dots; i++)
        for (j = 0; j < i; j++)
            if (dots[i] && dots[j] &&
                dotpairs[i*g->num_dots+j] >= 0 &&
                dotpairs[j*g->num_dots+i] >= 0)
                dsf_merge(dsf, i, j);

    /*
     * Now look for the largest component.
     */
    size = 0;
    j = -1;
    for (i = 0; i < g->num_dots; i++) {
        int newsize;
        if (dots[i] && dsf_canonify(dsf, i) == i &&
            (newsize = dsf_size(dsf, i)) > size) {
            j = i;
            size = newsize;
        }
    }

    /*
     * Work out which faces we're going to keep (precisely those with
     * at least one dot in the same connected component as j) and
     * which dots (those required by any face we're keeping).
     *
     * At this point we reuse the 'dots' array to indicate the dots
     * we're keeping, rather than the ones that are landlocked.
     */
    faces = snewn(g->num_faces, int);
    for (i = 0; i < g->num_faces; i++)
        faces[i] = 0;
    for (i = 0; i < g->num_dots; i++)
        dots[i] = 0;
    for (i = 0; i < g->num_faces; i++) {
        grid_face *f = g->faces + i;
        int keep = FALSE;
        for (k = 0; k < f->order; k++)
            if (dsf_canonify(dsf, f->dots[k] - g->dots) == j)
                keep = TRUE;
        if (keep) {
            faces[i] = TRUE;
            for (k = 0; k < f->order; k++)
                dots[f->dots[k]-g->dots] = TRUE;
        }
    }

    /*
     * Work out the new indices of those faces and dots, when we
     * compact the arrays containing them.
     */
    for (i = newfaces = 0; i < g->num_faces; i++)
        faces[i] = (faces[i] ? newfaces++ : -1);
    for (i = newdots = 0; i < g->num_dots; i++)
        dots[i] = (dots[i] ? newdots++ : -1);

    /*
     * Free the dynamically allocated 'dots' pointer lists in faces
     * we're going to discard.
     */
    for (i = 0; i < g->num_faces; i++)
        if (faces[i] < 0)
            sfree(g->faces[i].dots);

    /*
     * Go through and compact the arrays.
     */
    for (i = 0; i < g->num_dots; i++)
        if (dots[i] >= 0) {
            grid_dot *dnew = g->dots + dots[i], *dold = g->dots + i;
            *dnew = *dold;             /* structure copy */
        }
    for (i = 0; i < g->num_faces; i++)
        if (faces[i] >= 0) {
            grid_face *fnew = g->faces + faces[i], *fold = g->faces + i;
            *fnew = *fold;             /* structure copy */
            for (j = 0; j < fnew->order; j++) {
                /*
                 * Reindex the dots in this face.
                 */
                k = fnew->dots[j] - g->dots;
                fnew->dots[j] = g->dots + dots[k];
            }
        }
    g->num_faces = newfaces;
    g->num_dots = newdots;

    sfree(dotpairs);
    sfree(dsf);
    sfree(dots);
    sfree(faces);
}

/* Input: grid has its dots and faces initialised:
 * - dots have (optionally) x and y coordinates, but no edges or faces
 * (pointers are NULL).
 * - edges not initialised at all
 * - faces initialised and know which dots they have (but no edges yet).  The
 * dots around each face are assumed to be clockwise.
 *
 * Output: grid is complete and valid with all relationships defined.
 */
static void grid_make_consistent(grid *g)
{
    int i;
    tree234 *incomplete_edges;
    grid_edge *next_new_edge; /* Where new edge will go into g->edges */

    grid_debug_basic(g);

    /* ====== Stage 1 ======
     * Generate edges
     */

    /* We know how many dots and faces there are, so we can find the exact
     * number of edges from Euler's polyhedral formula: F + V = E + 2 .
     * We use "-1", not "-2" here, because Euler's formula includes the
     * infinite face, which we don't count. */
    g->num_edges = g->num_faces + g->num_dots - 1;
    debug(("allocating room for %d edges\n", g->num_edges));
    g->edges = snewn(g->num_edges, grid_edge);
    next_new_edge = g->edges;

    /* Iterate over faces, and over each face's dots, generating edges as we
     * go.  As we find each new edge, we can immediately fill in the edge's
     * dots, but only one of the edge's faces.  Later on in the iteration, we
     * will find the same edge again (unless it's on the border), but we will
     * know the other face.
     * For efficiency, maintain a list of the incomplete edges, sorted by
     * their dots. */
    incomplete_edges = newtree234(grid_edge_bydots_cmpfn);
    for (i = 0; i < g->num_faces; i++) {
        grid_face *f = g->faces + i;
        int j;
        assert(f->order > 2);
        for (j = 0; j < f->order; j++) {
            grid_edge e; /* fake edge for searching */
            grid_edge *edge_found;
            int j2 = j + 1;
            if (j2 == f->order)
                j2 = 0;
            e.dot1 = f->dots[j];
            e.dot2 = f->dots[j2];
            /* Use del234 instead of find234, because we always want to
             * remove the edge if found */
            edge_found = del234(incomplete_edges, &e);
            if (edge_found) {
                /* This edge already added, so fill out missing face.
                 * Edge is already removed from incomplete_edges. */
                edge_found->face2 = f;
            } else {
                assert(next_new_edge - g->edges < g->num_edges);
                next_new_edge->dot1 = e.dot1;
                next_new_edge->dot2 = e.dot2;
                next_new_edge->face1 = f;
                next_new_edge->face2 = NULL; /* potentially infinite face */
                add234(incomplete_edges, next_new_edge);
                ++next_new_edge;
            }
        }
    }
    freetree234(incomplete_edges);
    
    /* ====== Stage 2 ======
     * For each face, build its edge list.
     */

    /* Allocate space for each edge list.  Can do this, because each face's
     * edge-list is the same size as its dot-list. */
    for (i = 0; i < g->num_faces; i++) {
        grid_face *f = g->faces + i;
        int j;
        f->edges = snewn(f->order, grid_edge*);
        /* Preload with NULLs, to help detect potential bugs. */
        for (j = 0; j < f->order; j++)
            f->edges[j] = NULL;
    }
    
    /* Iterate over each edge, and over both its faces.  Add this edge to
     * the face's edge-list, after finding where it should go in the
     * sequence. */
    for (i = 0; i < g->num_edges; i++) {
        grid_edge *e = g->edges + i;
        int j;
        for (j = 0; j < 2; j++) {
            grid_face *f = j ? e->face2 : e->face1;
            int k, k2;
            if (f == NULL) continue;
            /* Find one of the dots around the face */
            for (k = 0; k < f->order; k++) {
                if (f->dots[k] == e->dot1)
                    break; /* found dot1 */
            }
            assert(k != f->order); /* Must find the dot around this face */

            /* Labelling scheme: as we walk clockwise around the face,
             * starting at dot0 (f->dots[0]), we hit:
             * (dot0), edge0, dot1, edge1, dot2,...
             *
             *     0
             *  0-----1
             *        |
             *        |1
             *        |
             *  3-----2
             *     2
             *
             * Therefore, edgeK joins dotK and dot{K+1}
             */
            
            /* Around this face, either the next dot or the previous dot
             * must be e->dot2.  Otherwise the edge is wrong. */
            k2 = k + 1;
            if (k2 == f->order)
                k2 = 0;
            if (f->dots[k2] == e->dot2) {
                /* dot1(k) and dot2(k2) go clockwise around this face, so add
                 * this edge at position k (see diagram). */
                assert(f->edges[k] == NULL);
                f->edges[k] = e;
                continue;
            }
            /* Try previous dot */
            k2 = k - 1;
            if (k2 == -1)
                k2 = f->order - 1;
            if (f->dots[k2] == e->dot2) {
                /* dot1(k) and dot2(k2) go anticlockwise around this face. */
                assert(f->edges[k2] == NULL);
                f->edges[k2] = e;
                continue;
            }
            assert(!"Grid broken: bad edge-face relationship");
        }
    }

    /* ====== Stage 3 ======
     * For each dot, build its edge-list and face-list.
     */

    /* We don't know how many edges/faces go around each dot, so we can't
     * allocate the right space for these lists.  Pre-compute the sizes by
     * iterating over each edge and recording a tally against each dot. */
    for (i = 0; i < g->num_dots; i++) {
        g->dots[i].order = 0;
    }
    for (i = 0; i < g->num_edges; i++) {
        grid_edge *e = g->edges + i;
        ++(e->dot1->order);
        ++(e->dot2->order);
    }
    /* Now we have the sizes, pre-allocate the edge and face lists. */
    for (i = 0; i < g->num_dots; i++) {
        grid_dot *d = g->dots + i;
        int j;
        assert(d->order >= 2); /* sanity check */
        d->edges = snewn(d->order, grid_edge*);
        d->faces = snewn(d->order, grid_face*);
        for (j = 0; j < d->order; j++) {
            d->edges[j] = NULL;
            d->faces[j] = NULL;
        }
    }
    /* For each dot, need to find a face that touches it, so we can seed
     * the edge-face-edge-face process around each dot. */
    for (i = 0; i < g->num_faces; i++) {
        grid_face *f = g->faces + i;
        int j;
        for (j = 0; j < f->order; j++) {
            grid_dot *d = f->dots[j];
            d->faces[0] = f;
        }
    }
    /* Each dot now has a face in its first slot.  Generate the remaining
     * faces and edges around the dot, by searching both clockwise and
     * anticlockwise from the first face.  Need to do both directions,
     * because of the possibility of hitting the infinite face, which
     * blocks progress.  But there's only one such face, so we will
     * succeed in finding every edge and face this way. */
    for (i = 0; i < g->num_dots; i++) {
        grid_dot *d = g->dots + i;
        int current_face1 = 0; /* ascends clockwise */
        int current_face2 = 0; /* descends anticlockwise */
        
        /* Labelling scheme: as we walk clockwise around the dot, starting
         * at face0 (d->faces[0]), we hit:
         * (face0), edge0, face1, edge1, face2,...
         *
         *       0
         *       |
         *    0  |  1
         *       |
         *  -----d-----1
         *       |
         *       |  2
         *       |
         *       2
         *
         * So, for example, face1 should be joined to edge0 and edge1,
         * and those edges should appear in an anticlockwise sense around
         * that face (see diagram). */
 
        /* clockwise search */
        while (TRUE) {
            grid_face *f = d->faces[current_face1];
            grid_edge *e;
            int j;
            assert(f != NULL);
            /* find dot around this face */
            for (j = 0; j < f->order; j++) {
                if (f->dots[j] == d)
                    break;
            }
            assert(j != f->order); /* must find dot */
            
            /* Around f, required edge is anticlockwise from the dot.  See
             * the other labelling scheme higher up, for why we subtract 1
             * from j. */
            j--;
            if (j == -1)
                j = f->order - 1;
            e = f->edges[j];
            d->edges[current_face1] = e; /* set edge */
            current_face1++;
            if (current_face1 == d->order)
                break;
            else {
                /* set face */
                d->faces[current_face1] =
                    (e->face1 == f) ? e->face2 : e->face1;
                if (d->faces[current_face1] == NULL)
                    break; /* cannot progress beyond infinite face */
            }
        }
        /* If the clockwise search made it all the way round, don't need to
         * bother with the anticlockwise search. */
        if (current_face1 == d->order)
            continue; /* this dot is complete, move on to next dot */
        
        /* anticlockwise search */
        while (TRUE) {
            grid_face *f = d->faces[current_face2];
            grid_edge *e;
            int j;
            assert(f != NULL);
            /* find dot around this face */
            for (j = 0; j < f->order; j++) {
                if (f->dots[j] == d)
                    break;
            }
            assert(j != f->order); /* must find dot */
            
            /* Around f, required edge is clockwise from the dot. */
            e = f->edges[j];
            
            current_face2--;
            if (current_face2 == -1)
                current_face2 = d->order - 1;
            d->edges[current_face2] = e; /* set edge */

            /* set face */
            if (current_face2 == current_face1)
                break;
            d->faces[current_face2] =
                    (e->face1 == f) ? e->face2 : e->face1;
            /* There's only 1 infinite face, so we must get all the way
             * to current_face1 before we hit it. */
            assert(d->faces[current_face2]);
        }
    }

    /* ====== Stage 4 ======
     * Compute other grid settings
     */

    /* Bounding rectangle */
    for (i = 0; i < g->num_dots; i++) {
        grid_dot *d = g->dots + i;
        if (i == 0) {
            g->lowest_x = g->highest_x = d->x;
            g->lowest_y = g->highest_y = d->y;
        } else {
            g->lowest_x = min(g->lowest_x, d->x);
            g->highest_x = max(g->highest_x, d->x);
            g->lowest_y = min(g->lowest_y, d->y);
            g->highest_y = max(g->highest_y, d->y);
        }
    }

    grid_debug_derived(g);
}

/* Helpers for making grid-generation easier.  These functions are only
 * intended for use during grid generation. */

/* Comparison function for the (tree234) sorted dot list */
static int grid_point_cmp_fn(void *v1, void *v2)
{
    grid_dot *p1 = v1;
    grid_dot *p2 = v2;
    if (p1->y != p2->y)
        return p2->y - p1->y;
    else
        return p2->x - p1->x;
}
/* Add a new face to the grid, with its dot list allocated.
 * Assumes there's enough space allocated for the new face in grid->faces */
static void grid_face_add_new(grid *g, int face_size)
{
    int i;
    grid_face *new_face = g->faces + g->num_faces;
    new_face->order = face_size;
    new_face->dots = snewn(face_size, grid_dot*);
    for (i = 0; i < face_size; i++)
        new_face->dots[i] = NULL;
    new_face->edges = NULL;
    new_face->has_incentre = FALSE;
    g->num_faces++;
}
/* Assumes dot list has enough space */
static grid_dot *grid_dot_add_new(grid *g, int x, int y)
{
    grid_dot *new_dot = g->dots + g->num_dots;
    new_dot->order = 0;
    new_dot->edges = NULL;
    new_dot->faces = NULL;
    new_dot->x = x;
    new_dot->y = y;
    g->num_dots++;
    return new_dot;
}
/* Retrieve a dot with these (x,y) coordinates.  Either return an existing dot
 * in the dot_list, or add a new dot to the grid (and the dot_list) and
 * return that.
 * Assumes g->dots has enough capacity allocated */
static grid_dot *grid_get_dot(grid *g, tree234 *dot_list, int x, int y)
{
    grid_dot test, *ret;

    test.order = 0;
    test.edges = NULL;
    test.faces = NULL;
    test.x = x;
    test.y = y;
    ret = find234(dot_list, &test, NULL);
    if (ret)
        return ret;

    ret = grid_dot_add_new(g, x, y);
    add234(dot_list, ret);
    return ret;
}

/* Sets the last face of the grid to include this dot, at this position
 * around the face. Assumes num_faces is at least 1 (a new face has
 * previously been added, with the required number of dots allocated) */
static void grid_face_set_dot(grid *g, grid_dot *d, int position)
{
    grid_face *last_face = g->faces + g->num_faces - 1;
    last_face->dots[position] = d;
}

/*
 * Helper routines for grid_find_incentre.
 */
static int solve_2x2_matrix(double mx[4], double vin[2], double vout[2])
{
    double inv[4];
    double det;
    det = (mx[0]*mx[3] - mx[1]*mx[2]);
    if (det == 0)
        return FALSE;

    inv[0] = mx[3] / det;
    inv[1] = -mx[1] / det;
    inv[2] = -mx[2] / det;
    inv[3] = mx[0] / det;

    vout[0] = inv[0]*vin[0] + inv[1]*vin[1];
    vout[1] = inv[2]*vin[0] + inv[3]*vin[1];

    return TRUE;
}
static int solve_3x3_matrix(double mx[9], double vin[3], double vout[3])
{
    double inv[9];
    double det;

    det = (mx[0]*mx[4]*mx[8] + mx[1]*mx[5]*mx[6] + mx[2]*mx[3]*mx[7] -
           mx[0]*mx[5]*mx[7] - mx[1]*mx[3]*mx[8] - mx[2]*mx[4]*mx[6]);
    if (det == 0)
        return FALSE;

    inv[0] = (mx[4]*mx[8] - mx[5]*mx[7]) / det;
    inv[1] = (mx[2]*mx[7] - mx[1]*mx[8]) / det;
    inv[2] = (mx[1]*mx[5] - mx[2]*mx[4]) / det;
    inv[3] = (mx[5]*mx[6] - mx[3]*mx[8]) / det;
    inv[4] = (mx[0]*mx[8] - mx[2]*mx[6]) / det;
    inv[5] = (mx[2]*mx[3] - mx[0]*mx[5]) / det;
    inv[6] = (mx[3]*mx[7] - mx[4]*mx[6]) / det;
    inv[7] = (mx[1]*mx[6] - mx[0]*mx[7]) / det;
    inv[8] = (mx[0]*mx[4] - mx[1]*mx[3]) / det;

    vout[0] = inv[0]*vin[0] + inv[1]*vin[1] + inv[2]*vin[2];
    vout[1] = inv[3]*vin[0] + inv[4]*vin[1] + inv[5]*vin[2];
    vout[2] = inv[6]*vin[0] + inv[7]*vin[1] + inv[8]*vin[2];

    return TRUE;
}

void grid_find_incentre(grid_face *f)
{
    double xbest, ybest, bestdist;
    int i, j, k, m;
    grid_dot *edgedot1[3], *edgedot2[3];
    grid_dot *dots[3];
    int nedges, ndots;

    if (f->has_incentre)
        return;

    /*
     * Find the point in the polygon with the maximum distance to any
     * edge or corner.
     *
     * Such a point must exist which is in contact with at least three
     * edges and/or vertices. (Proof: if it's only in contact with two
     * edges and/or vertices, it can't even be at a _local_ maximum -
     * any such circle can always be expanded in some direction.) So
     * we iterate through all 3-subsets of the combined set of edges
     * and vertices; for each subset we generate one or two candidate
     * points that might be the incentre, and then we vet each one to
     * see if it's inside the polygon and what its maximum radius is.
     *
     * (There's one case which this algorithm will get noticeably
     * wrong, and that's when a continuum of equally good answers
     * exists due to parallel edges. Consider a long thin rectangle,
     * for instance, or a parallelogram. This algorithm will pick a
     * point near one end, and choose the end arbitrarily; obviously a
     * nicer point to choose would be in the centre. To fix this I
     * would have to introduce a special-case system which detected
     * parallel edges in advance, set aside all candidate points
     * generated using both edges in a parallel pair, and generated
     * some additional candidate points half way between them. Also,
     * of course, I'd have to cope with rounding error making such a
     * point look worse than one of its endpoints. So I haven't done
     * this for the moment, and will cross it if necessary when I come
     * to it.)
     *
     * We don't actually iterate literally over _edges_, in the sense
     * of grid_edge structures. Instead, we fill in edgedot1[] and
     * edgedot2[] with a pair of dots adjacent in the face's list of
     * vertices. This ensures that we get the edges in consistent
     * orientation, which we could not do from the grid structure
     * alone. (A moment's consideration of an order-3 vertex should
     * make it clear that if a notional arrow was written on each
     * edge, _at least one_ of the three faces bordering that vertex
     * would have to have the two arrows tip-to-tip or tail-to-tail
     * rather than tip-to-tail.)
     */
    nedges = ndots = 0;
    bestdist = 0;
    xbest = ybest = 0;

    for (i = 0; i+2 < 2*f->order; i++) {
        if (i < f->order) {
            edgedot1[nedges] = f->dots[i];
            edgedot2[nedges++] = f->dots[(i+1)%f->order];
        } else
            dots[ndots++] = f->dots[i - f->order];

        for (j = i+1; j+1 < 2*f->order; j++) {
            if (j < f->order) {
                edgedot1[nedges] = f->dots[j];
                edgedot2[nedges++] = f->dots[(j+1)%f->order];
            } else
                dots[ndots++] = f->dots[j - f->order];

            for (k = j+1; k < 2*f->order; k++) {
                double cx[2], cy[2];   /* candidate positions */
                int cn = 0;            /* number of candidates */

                if (k < f->order) {
                    edgedot1[nedges] = f->dots[k];
                    edgedot2[nedges++] = f->dots[(k+1)%f->order];
                } else
                    dots[ndots++] = f->dots[k - f->order];

                /*
                 * Find a point, or pair of points, equidistant from
                 * all the specified edges and/or vertices.
                 */
                if (nedges == 3) {
                    /*
                     * Three edges. This is a linear matrix equation:
                     * each row of the matrix represents the fact that
                     * the point (x,y) we seek is at distance r from
                     * that edge, and we solve three of those
                     * simultaneously to obtain x,y,r. (We ignore r.)
                     */
                    double matrix[9], vector[3], vector2[3];
                    int m;

                    for (m = 0; m < 3; m++) {
                        int x1 = edgedot1[m]->x, x2 = edgedot2[m]->x;
                        int y1 = edgedot1[m]->y, y2 = edgedot2[m]->y;
                        int dx = x2-x1, dy = y2-y1;

                        /*
                         * ((x,y) - (x1,y1)) . (dy,-dx) = r |(dx,dy)|
                         *
                         * => x dy - y dx - r |(dx,dy)| = (x1 dy - y1 dx)
                         */
                        matrix[3*m+0] = dy;
                        matrix[3*m+1] = -dx;
                        matrix[3*m+2] = -sqrt((double)dx*dx+(double)dy*dy);
                        vector[m] = (double)x1*dy - (double)y1*dx;
                    }

                    if (solve_3x3_matrix(matrix, vector, vector2)) {
                        cx[cn] = vector2[0];
                        cy[cn] = vector2[1];
                        cn++;
                    }
                } else if (nedges == 2) {
                    /*
                     * Two edges and a dot. This will end up in a
                     * quadratic equation.
                     *
                     * First, look at the two edges. Having our point
                     * be some distance r from both of them gives rise
                     * to a pair of linear equations in x,y,r of the
                     * form
                     *
                     *   (x-x1) dy - (y-y1) dx = r sqrt(dx^2+dy^2)
                     *
                     * We eliminate r between those equations to give
                     * us a single linear equation in x,y describing
                     * the locus of points equidistant from both lines
                     * - i.e. the angle bisector. 
                     *
                     * We then choose one of x,y to be a parameter t,
                     * and derive linear formulae for x,y,r in terms
                     * of t. This enables us to write down the
                     * circular equation (x-xd)^2+(y-yd)^2=r^2 as a
                     * quadratic in t; solving that and substituting
                     * in for x,y gives us two candidate points.
                     */
                    double eqs[2][4];  /* a,b,c,d : ax+by+cr=d */
                    double eq[3];      /* a,b,c: ax+by=c */
                    double xt[2], yt[2], rt[2]; /* a,b: {x,y,r}=at+b */
                    double q[3];                /* a,b,c: at^2+bt+c=0 */
                    double disc;

                    /* Find equations of the two input lines. */
                    for (m = 0; m < 2; m++) {
                        int x1 = edgedot1[m]->x, x2 = edgedot2[m]->x;
                        int y1 = edgedot1[m]->y, y2 = edgedot2[m]->y;
                        int dx = x2-x1, dy = y2-y1;

                        eqs[m][0] = dy;
                        eqs[m][1] = -dx;
                        eqs[m][2] = -sqrt(dx*dx+dy*dy);
                        eqs[m][3] = x1*dy - y1*dx;
                    }

                    /* Derive the angle bisector by eliminating r. */
                    eq[0] = eqs[0][0]*eqs[1][2] - eqs[1][0]*eqs[0][2];
                    eq[1] = eqs[0][1]*eqs[1][2] - eqs[1][1]*eqs[0][2];
                    eq[2] = eqs[0][3]*eqs[1][2] - eqs[1][3]*eqs[0][2];

                    /* Parametrise x and y in terms of some t. */
                    if (abs(eq[0]) < abs(eq[1])) {
                        /* Parameter is x. */
                        xt[0] = 1; xt[1] = 0;
                        yt[0] = -eq[0]/eq[1]; yt[1] = eq[2]/eq[1];
                    } else {
                        /* Parameter is y. */
                        yt[0] = 1; yt[1] = 0;
                        xt[0] = -eq[1]/eq[0]; xt[1] = eq[2]/eq[0];
                    }

                    /* Find a linear representation of r using eqs[0]. */
                    rt[0] = -(eqs[0][0]*xt[0] + eqs[0][1]*yt[0])/eqs[0][2];
                    rt[1] = (eqs[0][3] - eqs[0][0]*xt[1] -
                             eqs[0][1]*yt[1])/eqs[0][2];

                    /* Construct the quadratic equation. */
                    q[0] = -rt[0]*rt[0];
                    q[1] = -2*rt[0]*rt[1];
                    q[2] = -rt[1]*rt[1];
                    q[0] += xt[0]*xt[0];
                    q[1] += 2*xt[0]*(xt[1]-dots[0]->x);
                    q[2] += (xt[1]-dots[0]->x)*(xt[1]-dots[0]->x);
                    q[0] += yt[0]*yt[0];
                    q[1] += 2*yt[0]*(yt[1]-dots[0]->y);
                    q[2] += (yt[1]-dots[0]->y)*(yt[1]-dots[0]->y);

                    /* And solve it. */
                    disc = q[1]*q[1] - 4*q[0]*q[2];
                    if (disc >= 0) {
                        double t;

                        disc = sqrt(disc);

                        t = (-q[1] + disc) / (2*q[0]);
                        cx[cn] = xt[0]*t + xt[1];
                        cy[cn] = yt[0]*t + yt[1];
                        cn++;

                        t = (-q[1] - disc) / (2*q[0]);
                        cx[cn] = xt[0]*t + xt[1];
                        cy[cn] = yt[0]*t + yt[1];
                        cn++;
                    }
                } else if (nedges == 1) {
                    /*
                     * Two dots and an edge. This one's another
                     * quadratic equation.
                     *
                     * The point we want must lie on the perpendicular
                     * bisector of the two dots; that much is obvious.
                     * So we can construct a parametrisation of that
                     * bisecting line, giving linear formulae for x,y
                     * in terms of t. We can also express the distance
                     * from the edge as such a linear formula.
                     *
                     * Then we set that equal to the radius of the
                     * circle passing through the two points, which is
                     * a Pythagoras exercise; that gives rise to a
                     * quadratic in t, which we solve.
                     */
                    double xt[2], yt[2], rt[2]; /* a,b: {x,y,r}=at+b */
                    double q[3];                /* a,b,c: at^2+bt+c=0 */
                    double disc;
                    double halfsep;

                    /* Find parametric formulae for x,y. */
                    {
                        int x1 = dots[0]->x, x2 = dots[1]->x;
                        int y1 = dots[0]->y, y2 = dots[1]->y;
                        int dx = x2-x1, dy = y2-y1;
                        double d = sqrt((double)dx*dx + (double)dy*dy);

                        xt[1] = (x1+x2)/2.0;
                        yt[1] = (y1+y2)/2.0;
                        /* It's convenient if we have t at standard scale. */
                        xt[0] = -dy/d;
                        yt[0] = dx/d;

                        /* Also note down half the separation between
                         * the dots, for use in computing the circle radius. */
                        halfsep = 0.5*d;
                    }

                    /* Find a parametric formula for r. */
                    {
                        int x1 = edgedot1[0]->x, x2 = edgedot2[0]->x;
                        int y1 = edgedot1[0]->y, y2 = edgedot2[0]->y;
                        int dx = x2-x1, dy = y2-y1;
                        double d = sqrt((double)dx*dx + (double)dy*dy);
                        rt[0] = (xt[0]*dy - yt[0]*dx) / d;
                        rt[1] = ((xt[1]-x1)*dy - (yt[1]-y1)*dx) / d;
                    }

                    /* Construct the quadratic equation. */
                    q[0] = rt[0]*rt[0];
                    q[1] = 2*rt[0]*rt[1];
                    q[2] = rt[1]*rt[1];
                    q[0] -= 1;
                    q[2] -= halfsep*halfsep;

                    /* And solve it. */
                    disc = q[1]*q[1] - 4*q[0]*q[2];
                    if (disc >= 0) {
                        double t;

                        disc = sqrt(disc);

                        t = (-q[1] + disc) / (2*q[0]);
                        cx[cn] = xt[0]*t + xt[1];
                        cy[cn] = yt[0]*t + yt[1];
                        cn++;

                        t = (-q[1] - disc) / (2*q[0]);
                        cx[cn] = xt[0]*t + xt[1];
                        cy[cn] = yt[0]*t + yt[1];
                        cn++;
                    }
                } else if (nedges == 0) {
                    /*
                     * Three dots. This is another linear matrix
                     * equation, this time with each row of the matrix
                     * representing the perpendicular bisector between
                     * two of the points. Of course we only need two
                     * such lines to find their intersection, so we
                     * need only solve a 2x2 matrix equation.
                     */

                    double matrix[4], vector[2], vector2[2];
                    int m;

                    for (m = 0; m < 2; m++) {
                        int x1 = dots[m]->x, x2 = dots[m+1]->x;
                        int y1 = dots[m]->y, y2 = dots[m+1]->y;
                        int dx = x2-x1, dy = y2-y1;

                        /*
                         * ((x,y) - (x1,y1)) . (dx,dy) = 1/2 |(dx,dy)|^2
                         *
                         * => 2x dx + 2y dy = dx^2+dy^2 + (2 x1 dx + 2 y1 dy)
                         */
                        matrix[2*m+0] = 2*dx;
                        matrix[2*m+1] = 2*dy;
                        vector[m] = ((double)dx*dx + (double)dy*dy +
                                     2.0*x1*dx + 2.0*y1*dy);
                    }

                    if (solve_2x2_matrix(matrix, vector, vector2)) {
                        cx[cn] = vector2[0];
                        cy[cn] = vector2[1];
                        cn++;
                    }
                }

                /*
                 * Now go through our candidate points and see if any
                 * of them are better than what we've got so far.
                 */
                for (m = 0; m < cn; m++) {
                    double x = cx[m], y = cy[m];

                    /*
                     * First, disqualify the point if it's not inside
                     * the polygon, which we work out by counting the
                     * edges to the right of the point. (For
                     * tiebreaking purposes when edges start or end on
                     * our y-coordinate or go right through it, we
                     * consider our point to be offset by a small
                     * _positive_ epsilon in both the x- and
                     * y-direction.)
                     */
                    int e, in = 0;
                    for (e = 0; e < f->order; e++) {
                        int xs = f->edges[e]->dot1->x;
                        int xe = f->edges[e]->dot2->x;
                        int ys = f->edges[e]->dot1->y;
                        int ye = f->edges[e]->dot2->y;
                        if ((y >= ys && y < ye) || (y >= ye && y < ys)) {
                            /*
                             * The line goes past our y-position. Now we need
                             * to know if its x-coordinate when it does so is
                             * to our right.
                             *
                             * The x-coordinate in question is mathematically
                             * (y - ys) * (xe - xs) / (ye - ys), and we want
                             * to know whether (x - xs) >= that. Of course we
                             * avoid the division, so we can work in integers;
                             * to do this we must multiply both sides of the
                             * inequality by ye - ys, which means we must
                             * first check that's not negative.
                             */
                            int num = xe - xs, denom = ye - ys;
                            if (denom < 0) {
                                num = -num;
                                denom = -denom;
                            }
                            if ((x - xs) * denom >= (y - ys) * num)
                                in ^= 1;
                        }
                    }

                    if (in) {
#ifdef HUGE_VAL
                        double mindist = HUGE_VAL;
#else
#ifdef DBL_MAX
                        double mindist = DBL_MAX;
#else
#error No way to get maximum floating-point number.
#endif
#endif
                        int e, d;

                        /*
                         * This point is inside the polygon, so now we check
                         * its minimum distance to every edge and corner.
                         * First the corners ...
                         */
                        for (d = 0; d < f->order; d++) {
                            int xp = f->dots[d]->x;
                            int yp = f->dots[d]->y;
                            double dx = x - xp, dy = y - yp;
                            double dist = dx*dx + dy*dy;
                            if (mindist > dist)
                                mindist = dist;
                        }

                        /*
                         * ... and now also check the perpendicular distance
                         * to every edge, if the perpendicular lies between
                         * the edge's endpoints.
                         */
                        for (e = 0; e < f->order; e++) {
                            int xs = f->edges[e]->dot1->x;
                            int xe = f->edges[e]->dot2->x;
                            int ys = f->edges[e]->dot1->y;
                            int ye = f->edges[e]->dot2->y;

                            /*
                             * If s and e are our endpoints, and p our
                             * candidate circle centre, the foot of a
                             * perpendicular from p to the line se lies
                             * between s and e if and only if (p-s).(e-s) lies
                             * strictly between 0 and (e-s).(e-s).
                             */
                            int edx = xe - xs, edy = ye - ys;
                            double pdx = x - xs, pdy = y - ys;
                            double pde = pdx * edx + pdy * edy;
                            long ede = (long)edx * edx + (long)edy * edy;
                            if (0 < pde && pde < ede) {
                                /*
                                 * Yes, the nearest point on this edge is
                                 * closer than either endpoint, so we must
                                 * take it into account by measuring the
                                 * perpendicular distance to the edge and
                                 * checking its square against mindist.
                                 */

                                double pdre = pdx * edy - pdy * edx;
                                double sqlen = pdre * pdre / ede;

                                if (mindist > sqlen)
                                    mindist = sqlen;
                            }
                        }

                        /*
                         * Right. Now we know the biggest circle around this
                         * point, so we can check it against bestdist.
                         */
                        if (bestdist < mindist) {
                            bestdist = mindist;
                            xbest = x;
                            ybest = y;
                        }
                    }
                }

                if (k < f->order)
                    nedges--;
                else
                    ndots--;
            }
            if (j < f->order)
                nedges--;
            else
                ndots--;
        }
        if (i < f->order)
            nedges--;
        else
            ndots--;
    }

    assert(bestdist > 0);

    f->has_incentre = TRUE;
    f->ix = xbest + 0.5;               /* round to nearest */
    f->iy = ybest + 0.5;
}

/* Generate the dual to a grid
 * Returns a new dynamically-allocated grid whose dots are the
 * faces of the input, and whose faces are the dots of the input.
 * A few modifications are made: dots on input that have only two
 * edges are deleted, and the infinite exterior face is also removed
 * before conversion.
 */
static grid *grid_dual(grid *g)
{
    grid *new_g;
    int i, j, k;
    tree234* points;

    new_g = grid_empty();
    new_g->tilesize = g->tilesize;
    new_g->faces = snewn(g->num_dots, grid_face);
    new_g->dots = snewn(g->num_faces, grid_dot);
    debug(("taking the dual of a grid with %d faces and %d dots\n",
                g->num_faces,g->num_dots));

    points = newtree234(grid_point_cmp_fn);

    for (i=0;i<g->num_faces;i++)
    {
        grid_find_incentre(&(g->faces[i]));
    }
    for (i=0;i<g->num_dots;i++)
    {
        int order;
        grid_dot *d;

        d = &(g->dots[i]);

        /* Detect and remove faces bordering the exterior */
        order = d->order;
        for (j=0;j<d->order;j++)
        {
            if (!d->faces[j]) order--;
        }
        if (order==d->order)
        {
            grid_face_add_new(new_g, order);
            for (j=0,k=0;j<d->order;j++)
            {
                grid_dot *new_d;
                if (d->faces[j])
                {
                    new_d = grid_get_dot(new_g, points,
                            d->faces[j]->ix, d->faces[j]->iy);
                    grid_face_set_dot(new_g, new_d, k++);
                }
            }
            assert(k==order);
        }
    }

    freetree234(points);
    assert(new_g->num_faces <= g->num_dots);
    assert(new_g->num_dots <= g->num_faces);

    debug(("dual has %d faces and %d dots\n",
                new_g->num_faces,new_g->num_dots));
    grid_trim_vigorously(new_g);
    grid_make_consistent(new_g);
    return new_g;
}
/* ------ Generate various types of grid ------ */

/* General method is to generate faces, by calculating their dot coordinates.
 * As new faces are added, we keep track of all the dots so we can tell when
 * a new face reuses an existing dot.  For example, two squares touching at an
 * edge would generate six unique dots: four dots from the first face, then
 * two additional dots for the second face, because we detect the other two
 * dots have already been taken up.  This list is stored in a tree234
 * called "points".  No extra memory-allocation needed here - we store the
 * actual grid_dot* pointers, which all point into the g->dots list.
 * For this reason, we have to calculate coordinates in such a way as to
 * eliminate any rounding errors, so we can detect when a dot on one
 * face precisely lands on a dot of a different face.  No floating-point
 * arithmetic here!
 */

#define SQUARE_TILESIZE 20

static void grid_size_square(int width, int height,
                      int *tilesize, int *xextent, int *yextent)
{
    int a = SQUARE_TILESIZE;

    *tilesize = a;
    *xextent = width * a;
    *yextent = height * a;
}

static grid *grid_new_square(int width, int height, char *desc)
{
    int x, y;
    /* Side length */
    int a = SQUARE_TILESIZE;

    /* Upper bounds - don't have to be exact */
    int max_faces = width * height;
    int max_dots = (width + 1) * (height + 1);

    tree234 *points;

    grid *g = grid_empty();
    g->tilesize = a;
    g->faces = snewn(max_faces, grid_face);
    g->dots = snewn(max_dots, grid_dot);

    points = newtree234(grid_point_cmp_fn);

    /* generate square faces */
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            grid_dot *d;
            /* face position */
            int px = a * x;
            int py = a * y;

            grid_face_add_new(g, 4);
            d = grid_get_dot(g, points, px, py);
            grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, px + a, py);
            grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, px + a, py + a);
            grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, px, py + a);
            grid_face_set_dot(g, d, 3);
        }
    }

    freetree234(points);
    assert(g->num_faces <= max_faces);
    assert(g->num_dots <= max_dots);

    grid_make_consistent(g);
    return g;
}

#define HONEY_TILESIZE 45
/* Vector for side of hexagon - ratio is close to sqrt(3) */
#define HONEY_A 15
#define HONEY_B 26

static void grid_size_honeycomb(int width, int height,
                         int *tilesize, int *xextent, int *yextent)
{
    int a = HONEY_A;
    int b = HONEY_B;

    *tilesize = HONEY_TILESIZE;
    *xextent = (3 * a * (width-1)) + 4*a;
    *yextent = (2 * b * (height-1)) + 3*b;
}

static grid *grid_new_honeycomb(int width, int height, char *desc)
{
    int x, y;
    int a = HONEY_A;
    int b = HONEY_B;

    /* Upper bounds - don't have to be exact */
    int max_faces = width * height;
    int max_dots = 2 * (width + 1) * (height + 1);

    tree234 *points;

    grid *g = grid_empty();
    g->tilesize = HONEY_TILESIZE;
    g->faces = snewn(max_faces, grid_face);
    g->dots = snewn(max_dots, grid_dot);

    points = newtree234(grid_point_cmp_fn);

    /* generate hexagonal faces */
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            grid_dot *d;
            /* face centre */
            int cx = 3 * a * x;
            int cy = 2 * b * y;
            if (x % 2)
                cy += b;
            grid_face_add_new(g, 6);

            d = grid_get_dot(g, points, cx - a, cy - b);
            grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, cx + a, cy - b);
            grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, cx + 2*a, cy);
            grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, cx + a, cy + b);
            grid_face_set_dot(g, d, 3);
            d = grid_get_dot(g, points, cx - a, cy + b);
            grid_face_set_dot(g, d, 4);
            d = grid_get_dot(g, points, cx - 2*a, cy);
            grid_face_set_dot(g, d, 5);
        }
    }

    freetree234(points);
    assert(g->num_faces <= max_faces);
    assert(g->num_dots <= max_dots);

    grid_make_consistent(g);
    return g;
}

#define TRIANGLE_TILESIZE 18
/* Vector for side of triangle - ratio is close to sqrt(3) */
#define TRIANGLE_VEC_X 15
#define TRIANGLE_VEC_Y 26

static void grid_size_triangular(int width, int height,
                          int *tilesize, int *xextent, int *yextent)
{
    int vec_x = TRIANGLE_VEC_X;
    int vec_y = TRIANGLE_VEC_Y;

    *tilesize = TRIANGLE_TILESIZE;
    *xextent = width * 2 * vec_x + vec_x;
    *yextent = height * vec_y;
}

/* Doesn't use the previous method of generation, it pre-dates it!
 * A triangular grid is just about simple enough to do by "brute force" */
static grid *grid_new_triangular(int width, int height, char *desc)
{
    int x,y;
    
    /* Vector for side of triangle - ratio is close to sqrt(3) */
    int vec_x = TRIANGLE_VEC_X;
    int vec_y = TRIANGLE_VEC_Y;
    
    int index;

    /* convenient alias */
    int w = width + 1;

    grid *g = grid_empty();
    g->tilesize = TRIANGLE_TILESIZE;

    g->num_faces = width * height * 2;
    g->num_dots = (width + 1) * (height + 1);
    g->faces = snewn(g->num_faces, grid_face);
    g->dots = snewn(g->num_dots, grid_dot);

    /* generate dots */
    index = 0;
    for (y = 0; y <= height; y++) {
        for (x = 0; x <= width; x++) {
            grid_dot *d = g->dots + index;
            /* odd rows are offset to the right */
            d->order = 0;
            d->edges = NULL;
            d->faces = NULL;
            d->x = x * 2 * vec_x + ((y % 2) ? vec_x : 0);
            d->y = y * vec_y;
            index++;
        }
    }
    
    /* generate faces */
    index = 0;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            /* initialise two faces for this (x,y) */
            grid_face *f1 = g->faces + index;
            grid_face *f2 = f1 + 1;
            f1->edges = NULL;
            f1->order = 3;
            f1->dots = snewn(f1->order, grid_dot*);
            f1->has_incentre = FALSE;
            f2->edges = NULL;
            f2->order = 3;
            f2->dots = snewn(f2->order, grid_dot*);
            f2->has_incentre = FALSE;

            /* face descriptions depend on whether the row-number is
             * odd or even */
            if (y % 2) {
                f1->dots[0] = g->dots + y       * w + x;
                f1->dots[1] = g->dots + (y + 1) * w + x + 1;
                f1->dots[2] = g->dots + (y + 1) * w + x;
                f2->dots[0] = g->dots + y       * w + x;
                f2->dots[1] = g->dots + y       * w + x + 1;
                f2->dots[2] = g->dots + (y + 1) * w + x + 1;
            } else {
                f1->dots[0] = g->dots + y       * w + x;
                f1->dots[1] = g->dots + y       * w + x + 1;
                f1->dots[2] = g->dots + (y + 1) * w + x;
                f2->dots[0] = g->dots + y       * w + x + 1;
                f2->dots[1] = g->dots + (y + 1) * w + x + 1;
                f2->dots[2] = g->dots + (y + 1) * w + x;
            }
            index += 2;
        }
    }

    grid_make_consistent(g);
    return g;
}

#define SNUBSQUARE_TILESIZE 18
/* Vector for side of triangle - ratio is close to sqrt(3) */
#define SNUBSQUARE_A 15
#define SNUBSQUARE_B 26

static void grid_size_snubsquare(int width, int height,
                          int *tilesize, int *xextent, int *yextent)
{
    int a = SNUBSQUARE_A;
    int b = SNUBSQUARE_B;

    *tilesize = SNUBSQUARE_TILESIZE;
    *xextent = (a+b) * (width-1) + a + b;
    *yextent = (a+b) * (height-1) + a + b;
}

static grid *grid_new_snubsquare(int width, int height, char *desc)
{
    int x, y;
    int a = SNUBSQUARE_A;
    int b = SNUBSQUARE_B;

    /* Upper bounds - don't have to be exact */
    int max_faces = 3 * width * height;
    int max_dots = 2 * (width + 1) * (height + 1);

    tree234 *points;

    grid *g = grid_empty();
    g->tilesize = SNUBSQUARE_TILESIZE;
    g->faces = snewn(max_faces, grid_face);
    g->dots = snewn(max_dots, grid_dot);

    points = newtree234(grid_point_cmp_fn);

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            grid_dot *d;
            /* face position */
            int px = (a + b) * x;
            int py = (a + b) * y;

            /* generate square faces */
            grid_face_add_new(g, 4);
            if ((x + y) % 2) {
                d = grid_get_dot(g, points, px + a, py);
                grid_face_set_dot(g, d, 0);
                d = grid_get_dot(g, points, px + a + b, py + a);
                grid_face_set_dot(g, d, 1);
                d = grid_get_dot(g, points, px + b, py + a + b);
                grid_face_set_dot(g, d, 2);
                d = grid_get_dot(g, points, px, py + b);
                grid_face_set_dot(g, d, 3);
            } else {
                d = grid_get_dot(g, points, px + b, py);
                grid_face_set_dot(g, d, 0);
                d = grid_get_dot(g, points, px + a + b, py + b);
                grid_face_set_dot(g, d, 1);
                d = grid_get_dot(g, points, px + a, py + a + b);
                grid_face_set_dot(g, d, 2);
                d = grid_get_dot(g, points, px, py + a);
                grid_face_set_dot(g, d, 3);
            }

            /* generate up/down triangles */
            if (x > 0) {
                grid_face_add_new(g, 3);
                if ((x + y) % 2) {
                    d = grid_get_dot(g, points, px + a, py);
                    grid_face_set_dot(g, d, 0);
                    d = grid_get_dot(g, points, px, py + b);
                    grid_face_set_dot(g, d, 1);
                    d = grid_get_dot(g, points, px - a, py);
                    grid_face_set_dot(g, d, 2);
                } else {
                    d = grid_get_dot(g, points, px, py + a);
                    grid_face_set_dot(g, d, 0);
                    d = grid_get_dot(g, points, px + a, py + a + b);
                    grid_face_set_dot(g, d, 1);
                    d = grid_get_dot(g, points, px - a, py + a + b);
                    grid_face_set_dot(g, d, 2);
                }
            }

            /* generate left/right triangles */
            if (y > 0) {
                grid_face_add_new(g, 3);
                if ((x + y) % 2) {
                    d = grid_get_dot(g, points, px + a, py);
                    grid_face_set_dot(g, d, 0);
                    d = grid_get_dot(g, points, px + a + b, py - a);
                    grid_face_set_dot(g, d, 1);
                    d = grid_get_dot(g, points, px + a + b, py + a);
                    grid_face_set_dot(g, d, 2);
                } else {
                    d = grid_get_dot(g, points, px, py - a);
                    grid_face_set_dot(g, d, 0);
                    d = grid_get_dot(g, points, px + b, py);
                    grid_face_set_dot(g, d, 1);
                    d = grid_get_dot(g, points, px, py + a);
                    grid_face_set_dot(g, d, 2);
                }
            }
        }
    }

    freetree234(points);
    assert(g->num_faces <= max_faces);
    assert(g->num_dots <= max_dots);

    grid_make_consistent(g);
    return g;
}

#define CAIRO_TILESIZE 40
/* Vector for side of pentagon - ratio is close to (4+sqrt(7))/3 */
#define CAIRO_A 14
#define CAIRO_B 31

static void grid_size_cairo(int width, int height,
                          int *tilesize, int *xextent, int *yextent)
{
    int b = CAIRO_B; /* a unused in determining grid size. */

    *tilesize = CAIRO_TILESIZE;
    *xextent = 2*b*(width-1) + 2*b;
    *yextent = 2*b*(height-1) + 2*b;
}

static grid *grid_new_cairo(int width, int height, char *desc)
{
    int x, y;
    int a = CAIRO_A;
    int b = CAIRO_B;

    /* Upper bounds - don't have to be exact */
    int max_faces = 2 * width * height;
    int max_dots = 3 * (width + 1) * (height + 1);

    tree234 *points;

    grid *g = grid_empty();
    g->tilesize = CAIRO_TILESIZE;
    g->faces = snewn(max_faces, grid_face);
    g->dots = snewn(max_dots, grid_dot);

    points = newtree234(grid_point_cmp_fn);

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            grid_dot *d;
            /* cell position */
            int px = 2 * b * x;
            int py = 2 * b * y;

            /* horizontal pentagons */
            if (y > 0) {
                grid_face_add_new(g, 5);
                if ((x + y) % 2) {
                    d = grid_get_dot(g, points, px + a, py - b);
                    grid_face_set_dot(g, d, 0);
                    d = grid_get_dot(g, points, px + 2*b - a, py - b);
                    grid_face_set_dot(g, d, 1);
                    d = grid_get_dot(g, points, px + 2*b, py);
                    grid_face_set_dot(g, d, 2);
                    d = grid_get_dot(g, points, px + b, py + a);
                    grid_face_set_dot(g, d, 3);
                    d = grid_get_dot(g, points, px, py);
                    grid_face_set_dot(g, d, 4);
                } else {
                    d = grid_get_dot(g, points, px, py);
                    grid_face_set_dot(g, d, 0);
                    d = grid_get_dot(g, points, px + b, py - a);
                    grid_face_set_dot(g, d, 1);
                    d = grid_get_dot(g, points, px + 2*b, py);
                    grid_face_set_dot(g, d, 2);
                    d = grid_get_dot(g, points, px + 2*b - a, py + b);
                    grid_face_set_dot(g, d, 3);
                    d = grid_get_dot(g, points, px + a, py + b);
                    grid_face_set_dot(g, d, 4);
                }
            }
            /* vertical pentagons */
            if (x > 0) {
                grid_face_add_new(g, 5);
                if ((x + y) % 2) {
                    d = grid_get_dot(g, points, px, py);
                    grid_face_set_dot(g, d, 0);
                    d = grid_get_dot(g, points, px + b, py + a);
                    grid_face_set_dot(g, d, 1);
                    d = grid_get_dot(g, points, px + b, py + 2*b - a);
                    grid_face_set_dot(g, d, 2);
                    d = grid_get_dot(g, points, px, py + 2*b);
                    grid_face_set_dot(g, d, 3);
                    d = grid_get_dot(g, points, px - a, py + b);
                    grid_face_set_dot(g, d, 4);
                } else {
                    d = grid_get_dot(g, points, px, py);
                    grid_face_set_dot(g, d, 0);
                    d = grid_get_dot(g, points, px + a, py + b);
                    grid_face_set_dot(g, d, 1);
                    d = grid_get_dot(g, points, px, py + 2*b);
                    grid_face_set_dot(g, d, 2);
                    d = grid_get_dot(g, points, px - b, py + 2*b - a);
                    grid_face_set_dot(g, d, 3);
                    d = grid_get_dot(g, points, px - b, py + a);
                    grid_face_set_dot(g, d, 4);
                }
            }
        }
    }

    freetree234(points);
    assert(g->num_faces <= max_faces);
    assert(g->num_dots <= max_dots);

    grid_make_consistent(g);
    return g;
}

#define GREATHEX_TILESIZE 18
/* Vector for side of triangle - ratio is close to sqrt(3) */
#define GREATHEX_A 15
#define GREATHEX_B 26

static void grid_size_greathexagonal(int width, int height,
                          int *tilesize, int *xextent, int *yextent)
{
    int a = GREATHEX_A;
    int b = GREATHEX_B;

    *tilesize = GREATHEX_TILESIZE;
    *xextent = (3*a + b) * (width-1) + 4*a;
    *yextent = (2*a + 2*b) * (height-1) + 3*b + a;
}

static grid *grid_new_greathexagonal(int width, int height, char *desc)
{
    int x, y;
    int a = GREATHEX_A;
    int b = GREATHEX_B;

    /* Upper bounds - don't have to be exact */
    int max_faces = 6 * (width + 1) * (height + 1);
    int max_dots = 6 * width * height;

    tree234 *points;

    grid *g = grid_empty();
    g->tilesize = GREATHEX_TILESIZE;
    g->faces = snewn(max_faces, grid_face);
    g->dots = snewn(max_dots, grid_dot);

    points = newtree234(grid_point_cmp_fn);

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            grid_dot *d;
            /* centre of hexagon */
            int px = (3*a + b) * x;
            int py = (2*a + 2*b) * y;
            if (x % 2)
                py += a + b;

            /* hexagon */
            grid_face_add_new(g, 6);
            d = grid_get_dot(g, points, px - a, py - b);
            grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, px + a, py - b);
            grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, px + 2*a, py);
            grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, px + a, py + b);
            grid_face_set_dot(g, d, 3);
            d = grid_get_dot(g, points, px - a, py + b);
            grid_face_set_dot(g, d, 4);
            d = grid_get_dot(g, points, px - 2*a, py);
            grid_face_set_dot(g, d, 5);

            /* square below hexagon */
            if (y < height - 1) {
                grid_face_add_new(g, 4);
                d = grid_get_dot(g, points, px - a, py + b);
                grid_face_set_dot(g, d, 0);
                d = grid_get_dot(g, points, px + a, py + b);
                grid_face_set_dot(g, d, 1);
                d = grid_get_dot(g, points, px + a, py + 2*a + b);
                grid_face_set_dot(g, d, 2);
                d = grid_get_dot(g, points, px - a, py + 2*a + b);
                grid_face_set_dot(g, d, 3);
            }

            /* square below right */
            if ((x < width - 1) && (((x % 2) == 0) || (y < height - 1))) {
                grid_face_add_new(g, 4);
                d = grid_get_dot(g, points, px + 2*a, py);
                grid_face_set_dot(g, d, 0);
                d = grid_get_dot(g, points, px + 2*a + b, py + a);
                grid_face_set_dot(g, d, 1);
                d = grid_get_dot(g, points, px + a + b, py + a + b);
                grid_face_set_dot(g, d, 2);
                d = grid_get_dot(g, points, px + a, py + b);
                grid_face_set_dot(g, d, 3);
            }

            /* square below left */
            if ((x > 0) && (((x % 2) == 0) || (y < height - 1))) {
                grid_face_add_new(g, 4);
                d = grid_get_dot(g, points, px - 2*a, py);
                grid_face_set_dot(g, d, 0);
                d = grid_get_dot(g, points, px - a, py + b);
                grid_face_set_dot(g, d, 1);
                d = grid_get_dot(g, points, px - a - b, py + a + b);
                grid_face_set_dot(g, d, 2);
                d = grid_get_dot(g, points, px - 2*a - b, py + a);
                grid_face_set_dot(g, d, 3);
            }
           
            /* Triangle below right */
            if ((x < width - 1) && (y < height - 1)) {
                grid_face_add_new(g, 3);
                d = grid_get_dot(g, points, px + a, py + b);
                grid_face_set_dot(g, d, 0);
                d = grid_get_dot(g, points, px + a + b, py + a + b);
                grid_face_set_dot(g, d, 1);
                d = grid_get_dot(g, points, px + a, py + 2*a + b);
                grid_face_set_dot(g, d, 2);
            }

            /* Triangle below left */
            if ((x > 0) && (y < height - 1)) {
                grid_face_add_new(g, 3);
                d = grid_get_dot(g, points, px - a, py + b);
                grid_face_set_dot(g, d, 0);
                d = grid_get_dot(g, points, px - a, py + 2*a + b);
                grid_face_set_dot(g, d, 1);
                d = grid_get_dot(g, points, px - a - b, py + a + b);
                grid_face_set_dot(g, d, 2);
            }
        }
    }

    freetree234(points);
    assert(g->num_faces <= max_faces);
    assert(g->num_dots <= max_dots);

    grid_make_consistent(g);
    return g;
}
#define OCTAGONAL_TILESIZE 40
/* b/a approx sqrt(2) */
#define OCTAGONAL_A 29
#define OCTAGONAL_B 41

static void grid_size_octagonal(int width, int height,
                          int *tilesize, int *xextent, int *yextent)
{
    int a = OCTAGONAL_A;
    int b = OCTAGONAL_B;

    *tilesize = OCTAGONAL_TILESIZE;
    *xextent = (2*a + b) * width;
    *yextent = (2*a + b) * height;
}

static grid *grid_new_octagonal(int width, int height, char *desc)
{
    int x, y;
    int a = OCTAGONAL_A;
    int b = OCTAGONAL_B;

    /* Upper bounds - don't have to be exact */
    int max_faces = 2 * width * height;
    int max_dots = 4 * (width + 1) * (height + 1);

    tree234 *points;

    grid *g = grid_empty();
    g->tilesize = OCTAGONAL_TILESIZE;
    g->faces = snewn(max_faces, grid_face);
    g->dots = snewn(max_dots, grid_dot);

    points = newtree234(grid_point_cmp_fn);

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            grid_dot *d;
            /* cell position */
            int px = (2*a + b) * x;
            int py = (2*a + b) * y;
            /* octagon */
            grid_face_add_new(g, 8);
            d = grid_get_dot(g, points, px + a, py);
            grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, px + a + b, py);
            grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, px + 2*a + b, py + a);
            grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, px + 2*a + b, py + a + b);
            grid_face_set_dot(g, d, 3);
            d = grid_get_dot(g, points, px + a + b, py + 2*a + b);
            grid_face_set_dot(g, d, 4);
            d = grid_get_dot(g, points, px + a, py + 2*a + b);
            grid_face_set_dot(g, d, 5);
            d = grid_get_dot(g, points, px, py + a + b);
            grid_face_set_dot(g, d, 6);
            d = grid_get_dot(g, points, px, py + a);
            grid_face_set_dot(g, d, 7);

            /* diamond */
            if ((x > 0) && (y > 0)) {
                grid_face_add_new(g, 4);
                d = grid_get_dot(g, points, px, py - a);
                grid_face_set_dot(g, d, 0);
                d = grid_get_dot(g, points, px + a, py);
                grid_face_set_dot(g, d, 1);
                d = grid_get_dot(g, points, px, py + a);
                grid_face_set_dot(g, d, 2);
                d = grid_get_dot(g, points, px - a, py);
                grid_face_set_dot(g, d, 3);
            }
        }
    }

    freetree234(points);
    assert(g->num_faces <= max_faces);
    assert(g->num_dots <= max_dots);

    grid_make_consistent(g);
    return g;
}

#define KITE_TILESIZE 40
/* b/a approx sqrt(3) */
#define KITE_A 15
#define KITE_B 26

static void grid_size_kites(int width, int height,
                     int *tilesize, int *xextent, int *yextent)
{
    int a = KITE_A;
    int b = KITE_B;

    *tilesize = KITE_TILESIZE;
    *xextent = 4*b * width + 2*b;
    *yextent = 6*a * (height-1) + 8*a;
}

static grid *grid_new_kites(int width, int height, char *desc)
{
    int x, y;
    int a = KITE_A;
    int b = KITE_B;

    /* Upper bounds - don't have to be exact */
    int max_faces = 6 * width * height;
    int max_dots = 6 * (width + 1) * (height + 1);

    tree234 *points;

    grid *g = grid_empty();
    g->tilesize = KITE_TILESIZE;
    g->faces = snewn(max_faces, grid_face);
    g->dots = snewn(max_dots, grid_dot);

    points = newtree234(grid_point_cmp_fn);

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            grid_dot *d;
            /* position of order-6 dot */
            int px = 4*b * x;
            int py = 6*a * y;
            if (y % 2)
                px += 2*b;

            /* kite pointing up-left */
            grid_face_add_new(g, 4);
            d = grid_get_dot(g, points, px, py);
            grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, px + 2*b, py);
            grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, px + 2*b, py + 2*a);
            grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, px + b, py + 3*a);
            grid_face_set_dot(g, d, 3);

            /* kite pointing up */
            grid_face_add_new(g, 4);
            d = grid_get_dot(g, points, px, py);
            grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, px + b, py + 3*a);
            grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, px, py + 4*a);
            grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, px - b, py + 3*a);
            grid_face_set_dot(g, d, 3);

            /* kite pointing up-right */
            grid_face_add_new(g, 4);
            d = grid_get_dot(g, points, px, py);
            grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, px - b, py + 3*a);
            grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, px - 2*b, py + 2*a);
            grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, px - 2*b, py);
            grid_face_set_dot(g, d, 3);

            /* kite pointing down-right */
            grid_face_add_new(g, 4);
            d = grid_get_dot(g, points, px, py);
            grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, px - 2*b, py);
            grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, px - 2*b, py - 2*a);
            grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, px - b, py - 3*a);
            grid_face_set_dot(g, d, 3);

            /* kite pointing down */
            grid_face_add_new(g, 4);
            d = grid_get_dot(g, points, px, py);
            grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, px - b, py - 3*a);
            grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, px, py - 4*a);
            grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, px + b, py - 3*a);
            grid_face_set_dot(g, d, 3);

            /* kite pointing down-left */
            grid_face_add_new(g, 4);
            d = grid_get_dot(g, points, px, py);
            grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, px + b, py - 3*a);
            grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, px + 2*b, py - 2*a);
            grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, px + 2*b, py);
            grid_face_set_dot(g, d, 3);
        }
    }

    freetree234(points);
    assert(g->num_faces <= max_faces);
    assert(g->num_dots <= max_dots);

    grid_make_consistent(g);
    return g;
}

#define FLORET_TILESIZE 150
/* -py/px is close to tan(30 - atan(sqrt(3)/9))
 * using py=26 makes everything lean to the left, rather than right
 */
#define FLORET_PX 75
#define FLORET_PY -26

static void grid_size_floret(int width, int height,
                          int *tilesize, int *xextent, int *yextent)
{
    int px = FLORET_PX, py = FLORET_PY;         /* |( 75, -26)| = 79.43 */
    int qx = 4*px/5, qy = -py*2;                /* |( 60,  52)| = 79.40 */
    int ry = qy-py;
    /* rx unused in determining grid size. */

    *tilesize = FLORET_TILESIZE;
    *xextent = (6*px+3*qx)/2 * (width-1) + 4*qx + 2*px;
    *yextent = (5*qy-4*py) * (height-1) + 4*qy + 2*ry;
}

static grid *grid_new_floret(int width, int height, char *desc)
{
    int x, y;
    /* Vectors for sides; weird numbers needed to keep puzzle aligned with window
     * -py/px is close to tan(30 - atan(sqrt(3)/9))
     * using py=26 makes everything lean to the left, rather than right
     */
    int px = FLORET_PX, py = FLORET_PY;         /* |( 75, -26)| = 79.43 */
    int qx = 4*px/5, qy = -py*2;                /* |( 60,  52)| = 79.40 */
    int rx = qx-px, ry = qy-py;                 /* |(-15,  78)| = 79.38 */

    /* Upper bounds - don't have to be exact */
    int max_faces = 6 * width * height;
    int max_dots = 9 * (width + 1) * (height + 1);

    tree234 *points;

    grid *g = grid_empty();
    g->tilesize = FLORET_TILESIZE;
    g->faces = snewn(max_faces, grid_face);
    g->dots = snewn(max_dots, grid_dot);

    points = newtree234(grid_point_cmp_fn);

    /* generate pentagonal faces */
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            grid_dot *d;
            /* face centre */
            int cx = (6*px+3*qx)/2 * x;
            int cy = (4*py-5*qy) * y;
            if (x % 2)
                cy -= (4*py-5*qy)/2;
            else if (y && y == height-1)
                continue; /* make better looking grids?  try 3x3 for instance */

            grid_face_add_new(g, 5);
            d = grid_get_dot(g, points, cx        , cy        ); grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, cx+2*rx   , cy+2*ry   ); grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, cx+2*rx+qx, cy+2*ry+qy); grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, cx+2*qx+rx, cy+2*qy+ry); grid_face_set_dot(g, d, 3);
            d = grid_get_dot(g, points, cx+2*qx   , cy+2*qy   ); grid_face_set_dot(g, d, 4);

            grid_face_add_new(g, 5);
            d = grid_get_dot(g, points, cx        , cy        ); grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, cx+2*qx   , cy+2*qy   ); grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, cx+2*qx+px, cy+2*qy+py); grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, cx+2*px+qx, cy+2*py+qy); grid_face_set_dot(g, d, 3);
            d = grid_get_dot(g, points, cx+2*px   , cy+2*py   ); grid_face_set_dot(g, d, 4);

            grid_face_add_new(g, 5);
            d = grid_get_dot(g, points, cx        , cy        ); grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, cx+2*px   , cy+2*py   ); grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, cx+2*px-rx, cy+2*py-ry); grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, cx-2*rx+px, cy-2*ry+py); grid_face_set_dot(g, d, 3);
            d = grid_get_dot(g, points, cx-2*rx   , cy-2*ry   ); grid_face_set_dot(g, d, 4);

            grid_face_add_new(g, 5);
            d = grid_get_dot(g, points, cx        , cy        ); grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, cx-2*rx   , cy-2*ry   ); grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, cx-2*rx-qx, cy-2*ry-qy); grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, cx-2*qx-rx, cy-2*qy-ry); grid_face_set_dot(g, d, 3);
            d = grid_get_dot(g, points, cx-2*qx   , cy-2*qy   ); grid_face_set_dot(g, d, 4);

            grid_face_add_new(g, 5);
            d = grid_get_dot(g, points, cx        , cy        ); grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, cx-2*qx   , cy-2*qy   ); grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, cx-2*qx-px, cy-2*qy-py); grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, cx-2*px-qx, cy-2*py-qy); grid_face_set_dot(g, d, 3);
            d = grid_get_dot(g, points, cx-2*px   , cy-2*py   ); grid_face_set_dot(g, d, 4);

            grid_face_add_new(g, 5);
            d = grid_get_dot(g, points, cx        , cy        ); grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, cx-2*px   , cy-2*py   ); grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, cx-2*px+rx, cy-2*py+ry); grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, cx+2*rx-px, cy+2*ry-py); grid_face_set_dot(g, d, 3);
            d = grid_get_dot(g, points, cx+2*rx   , cy+2*ry   ); grid_face_set_dot(g, d, 4);
        }
    }

    freetree234(points);
    assert(g->num_faces <= max_faces);
    assert(g->num_dots <= max_dots);

    grid_make_consistent(g);
    return g;
}

/* DODEC_* are used for dodecagonal and great-dodecagonal grids. */
#define DODEC_TILESIZE 26
/* Vector for side of triangle - ratio is close to sqrt(3) */
#define DODEC_A 15
#define DODEC_B 26

static void grid_size_dodecagonal(int width, int height,
                          int *tilesize, int *xextent, int *yextent)
{
    int a = DODEC_A;
    int b = DODEC_B;

    *tilesize = DODEC_TILESIZE;
    *xextent = (4*a + 2*b) * (width-1) + 3*(2*a + b);
    *yextent = (3*a + 2*b) * (height-1) + 2*(2*a + b);
}

static grid *grid_new_dodecagonal(int width, int height, char *desc)
{
    int x, y;
    int a = DODEC_A;
    int b = DODEC_B;

    /* Upper bounds - don't have to be exact */
    int max_faces = 3 * width * height;
    int max_dots = 14 * width * height;

    tree234 *points;

    grid *g = grid_empty();
    g->tilesize = DODEC_TILESIZE;
    g->faces = snewn(max_faces, grid_face);
    g->dots = snewn(max_dots, grid_dot);

    points = newtree234(grid_point_cmp_fn);

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            grid_dot *d;
            /* centre of dodecagon */
            int px = (4*a + 2*b) * x;
            int py = (3*a + 2*b) * y;
            if (y % 2)
                px += 2*a + b;

            /* dodecagon */
            grid_face_add_new(g, 12);
            d = grid_get_dot(g, points, px + (  a    ), py - (2*a + b)); grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, px + (  a + b), py - (  a + b)); grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, px + (2*a + b), py - (  a    )); grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, px + (2*a + b), py + (  a    )); grid_face_set_dot(g, d, 3);
            d = grid_get_dot(g, points, px + (  a + b), py + (  a + b)); grid_face_set_dot(g, d, 4);
            d = grid_get_dot(g, points, px + (  a    ), py + (2*a + b)); grid_face_set_dot(g, d, 5);
            d = grid_get_dot(g, points, px - (  a    ), py + (2*a + b)); grid_face_set_dot(g, d, 6);
            d = grid_get_dot(g, points, px - (  a + b), py + (  a + b)); grid_face_set_dot(g, d, 7);
            d = grid_get_dot(g, points, px - (2*a + b), py + (  a    )); grid_face_set_dot(g, d, 8);
            d = grid_get_dot(g, points, px - (2*a + b), py - (  a    )); grid_face_set_dot(g, d, 9);
            d = grid_get_dot(g, points, px - (  a + b), py - (  a + b)); grid_face_set_dot(g, d, 10);
            d = grid_get_dot(g, points, px - (  a    ), py - (2*a + b)); grid_face_set_dot(g, d, 11);

            /* triangle below dodecagon */
	    if ((y < height - 1 && (x < width - 1 || !(y % 2)) && (x > 0 || (y % 2)))) {
	      	grid_face_add_new(g, 3);
	      	d = grid_get_dot(g, points, px + a, py + (2*a +   b)); grid_face_set_dot(g, d, 0);
	      	d = grid_get_dot(g, points, px    , py + (2*a + 2*b)); grid_face_set_dot(g, d, 1);
	      	d = grid_get_dot(g, points, px - a, py + (2*a +   b)); grid_face_set_dot(g, d, 2);
	    }

            /* triangle above dodecagon */
	    if ((y && (x < width - 1 || !(y % 2)) && (x > 0 || (y % 2)))) {
	      	grid_face_add_new(g, 3);
	      	d = grid_get_dot(g, points, px - a, py - (2*a +   b)); grid_face_set_dot(g, d, 0);
	      	d = grid_get_dot(g, points, px    , py - (2*a + 2*b)); grid_face_set_dot(g, d, 1);
	      	d = grid_get_dot(g, points, px + a, py - (2*a +   b)); grid_face_set_dot(g, d, 2);
	    }
	}
    }

    freetree234(points);
    assert(g->num_faces <= max_faces);
    assert(g->num_dots <= max_dots);

    grid_make_consistent(g);
    return g;
}

static void grid_size_greatdodecagonal(int width, int height,
                          int *tilesize, int *xextent, int *yextent)
{
    int a = DODEC_A;
    int b = DODEC_B;

    *tilesize = DODEC_TILESIZE;
    *xextent = (6*a + 2*b) * (width-1) + 2*(2*a + b) + 3*a + b;
    *yextent = (3*a + 3*b) * (height-1) + 2*(2*a + b);
}

static grid *grid_new_greatdodecagonal(int width, int height, char *desc)
{
    int x, y;
    /* Vector for side of triangle - ratio is close to sqrt(3) */
    int a = DODEC_A;
    int b = DODEC_B;

    /* Upper bounds - don't have to be exact */
    int max_faces = 30 * width * height;
    int max_dots = 200 * width * height;

    tree234 *points;

    grid *g = grid_empty();
    g->tilesize = DODEC_TILESIZE;
    g->faces = snewn(max_faces, grid_face);
    g->dots = snewn(max_dots, grid_dot);

    points = newtree234(grid_point_cmp_fn);

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            grid_dot *d;
            /* centre of dodecagon */
            int px = (6*a + 2*b) * x;
            int py = (3*a + 3*b) * y;
            if (y % 2)
                px += 3*a + b;

            /* dodecagon */
            grid_face_add_new(g, 12);
            d = grid_get_dot(g, points, px + (  a    ), py - (2*a + b)); grid_face_set_dot(g, d, 0);
            d = grid_get_dot(g, points, px + (  a + b), py - (  a + b)); grid_face_set_dot(g, d, 1);
            d = grid_get_dot(g, points, px + (2*a + b), py - (  a    )); grid_face_set_dot(g, d, 2);
            d = grid_get_dot(g, points, px + (2*a + b), py + (  a    )); grid_face_set_dot(g, d, 3);
            d = grid_get_dot(g, points, px + (  a + b), py + (  a + b)); grid_face_set_dot(g, d, 4);
            d = grid_get_dot(g, points, px + (  a    ), py + (2*a + b)); grid_face_set_dot(g, d, 5);
            d = grid_get_dot(g, points, px - (  a    ), py + (2*a + b)); grid_face_set_dot(g, d, 6);
            d = grid_get_dot(g, points, px - (  a + b), py + (  a + b)); grid_face_set_dot(g, d, 7);
            d = grid_get_dot(g, points, px - (2*a + b), py + (  a    )); grid_face_set_dot(g, d, 8);
            d = grid_get_dot(g, points, px - (2*a + b), py - (  a    )); grid_face_set_dot(g, d, 9);
            d = grid_get_dot(g, points, px - (  a + b), py - (  a + b)); grid_face_set_dot(g, d, 10);
            d = grid_get_dot(g, points, px - (  a    ), py - (2*a + b)); grid_face_set_dot(g, d, 11);

            /* hexagon below dodecagon */
	    if (y < height - 1 && (x < width - 1 || !(y % 2)) && (x > 0 || (y % 2))) {
	      	grid_face_add_new(g, 6);
	      	d = grid_get_dot(g, points, px +   a, py + (2*a +   b)); grid_face_set_dot(g, d, 0);
	      	d = grid_get_dot(g, points, px + 2*a, py + (2*a + 2*b)); grid_face_set_dot(g, d, 1);
	      	d = grid_get_dot(g, points, px +   a, py + (2*a + 3*b)); grid_face_set_dot(g, d, 2);
	      	d = grid_get_dot(g, points, px -   a, py + (2*a + 3*b)); grid_face_set_dot(g, d, 3);
	      	d = grid_get_dot(g, points, px - 2*a, py + (2*a + 2*b)); grid_face_set_dot(g, d, 4);
	      	d = grid_get_dot(g, points, px -   a, py + (2*a +   b)); grid_face_set_dot(g, d, 5);
	    }

            /* hexagon above dodecagon */
	    if (y && (x < width - 1 || !(y % 2)) && (x > 0 || (y % 2))) {
	      	grid_face_add_new(g, 6);
	      	d = grid_get_dot(g, points, px -   a, py - (2*a +   b)); grid_face_set_dot(g, d, 0);
	      	d = grid_get_dot(g, points, px - 2*a, py - (2*a + 2*b)); grid_face_set_dot(g, d, 1);
	      	d = grid_get_dot(g, points, px -   a, py - (2*a + 3*b)); grid_face_set_dot(g, d, 2);
	      	d = grid_get_dot(g, points, px +   a, py - (2*a + 3*b)); grid_face_set_dot(g, d, 3);
	      	d = grid_get_dot(g, points, px + 2*a, py - (2*a + 2*b)); grid_face_set_dot(g, d, 4);
	      	d = grid_get_dot(g, points, px +   a, py - (2*a +   b)); grid_face_set_dot(g, d, 5);
	    }

            /* square on right of dodecagon */
	    if (x < width - 1) {
	      	grid_face_add_new(g, 4);
	      	d = grid_get_dot(g, points, px + 2*a + b, py - a); grid_face_set_dot(g, d, 0);
	      	d = grid_get_dot(g, points, px + 4*a + b, py - a); grid_face_set_dot(g, d, 1);
	      	d = grid_get_dot(g, points, px + 4*a + b, py + a); grid_face_set_dot(g, d, 2);
	      	d = grid_get_dot(g, points, px + 2*a + b, py + a); grid_face_set_dot(g, d, 3);
	    }

            /* square on top right of dodecagon */
	    if (y && (x < width - 1 || !(y % 2))) {
	      	grid_face_add_new(g, 4);
	      	d = grid_get_dot(g, points, px + (  a    ), py - (2*a +   b)); grid_face_set_dot(g, d, 0);
		d = grid_get_dot(g, points, px + (2*a    ), py - (2*a + 2*b)); grid_face_set_dot(g, d, 1);
		d = grid_get_dot(g, points, px + (2*a + b), py - (  a + 2*b)); grid_face_set_dot(g, d, 2);
		d = grid_get_dot(g, points, px + (  a + b), py - (  a +   b)); grid_face_set_dot(g, d, 3);
	    }

            /* square on top left of dodecagon */
	    if (y && (x || (y % 2))) {
	      	grid_face_add_new(g, 4);
		d = grid_get_dot(g, points, px - (  a + b), py - (  a +   b)); grid_face_set_dot(g, d, 0);
		d = grid_get_dot(g, points, px - (2*a + b), py - (  a + 2*b)); grid_face_set_dot(g, d, 1);
		d = grid_get_dot(g, points, px - (2*a    ), py - (2*a + 2*b)); grid_face_set_dot(g, d, 2);
	      	d = grid_get_dot(g, points, px - (  a    ), py - (2*a +   b)); grid_face_set_dot(g, d, 3);
	    }
	}
    }

    freetree234(points);
    assert(g->num_faces <= max_faces);
    assert(g->num_dots <= max_dots);

    grid_make_consistent(g);
    return g;
}

typedef struct setface_ctx
{
    int xmin, xmax, ymin, ymax;

    grid *g;
    tree234 *points;
} setface_ctx;

static double round_int_nearest_away(double r)
{
    return (r > 0.0) ? floor(r + 0.5) : ceil(r - 0.5);
}

static int set_faces(penrose_state *state, vector *vs, int n, int depth)
{
    setface_ctx *sf_ctx = (setface_ctx *)state->ctx;
    int i;
    int xs[4], ys[4];

    if (depth < state->max_depth) return 0;
#ifdef DEBUG_PENROSE
    if (n != 4) return 0; /* triangles are sent as debugging. */
#endif

    for (i = 0; i < n; i++) {
        double tx = v_x(vs, i), ty = v_y(vs, i);

        xs[i] = (int)round_int_nearest_away(tx);
        ys[i] = (int)round_int_nearest_away(ty);

        if (xs[i] < sf_ctx->xmin || xs[i] > sf_ctx->xmax) return 0;
        if (ys[i] < sf_ctx->ymin || ys[i] > sf_ctx->ymax) return 0;
    }

    grid_face_add_new(sf_ctx->g, n);
    debug(("penrose: new face l=%f gen=%d...",
           penrose_side_length(state->start_size, depth), depth));
    for (i = 0; i < n; i++) {
        grid_dot *d = grid_get_dot(sf_ctx->g, sf_ctx->points,
                                   xs[i], ys[i]);
        grid_face_set_dot(sf_ctx->g, d, i);
        debug((" ... dot 0x%x (%d,%d) (was %2.2f,%2.2f)",
               d, d->x, d->y, v_x(vs, i), v_y(vs, i)));
    }

    return 0;
}

#define PENROSE_TILESIZE 100

static void grid_size_penrose(int width, int height,
                       int *tilesize, int *xextent, int *yextent)
{
    int l = PENROSE_TILESIZE;

    *tilesize = l;
    *xextent = l * width;
    *yextent = l * height;
}

static char *grid_new_desc_penrose(grid_type type, int width, int height, random_state *rs)
{
    int tilesize = PENROSE_TILESIZE, startsz, depth, xoff, yoff, aoff;
    double outer_radius;
    int inner_radius;
    char gd[255];
    int which = (type == GRID_PENROSE_P2 ? PENROSE_P2 : PENROSE_P3);

    /* We want to produce a random bit of penrose tiling, so we calculate
     * a random offset (within the patch that penrose.c calculates for us)
     * and an angle (multiple of 36) to rotate the patch. */

    penrose_calculate_size(which, tilesize, width, height,
                           &outer_radius, &startsz, &depth);

    /* Calculate radius of (circumcircle of) patch, subtract from
     * radius calculated. */
    inner_radius = (int)(outer_radius - sqrt(width*width + height*height));

    /* Pick a random offset (the easy way: choose within outer square,
     * discarding while it's outside the circle) */
    do {
        xoff = random_upto(rs, 2*inner_radius) - inner_radius;
        yoff = random_upto(rs, 2*inner_radius) - inner_radius;
    } while (sqrt(xoff*xoff+yoff*yoff) > inner_radius);

    aoff = random_upto(rs, 360/36) * 36;

    debug(("grid_desc: ts %d, %dx%d patch, orad %2.2f irad %d",
           tilesize, width, height, outer_radius, inner_radius));
    debug(("    -> xoff %d yoff %d aoff %d", xoff, yoff, aoff));

    sprintf(gd, "G%d,%d,%d", xoff, yoff, aoff);

    return dupstr(gd);
}

static char *grid_validate_desc_penrose(grid_type type, int width, int height, char *desc)
{
    int tilesize = PENROSE_TILESIZE, startsz, depth, xoff, yoff, aoff, inner_radius;
    double outer_radius;
    int which = (type == GRID_PENROSE_P2 ? PENROSE_P2 : PENROSE_P3);

    if (!desc)
        return "Missing grid description string.";

    penrose_calculate_size(which, tilesize, width, height,
                           &outer_radius, &startsz, &depth);
    inner_radius = (int)(outer_radius - sqrt(width*width + height*height));

    if (sscanf(desc, "G%d,%d,%d", &xoff, &yoff, &aoff) != 3)
        return "Invalid format grid description string.";

    if (sqrt(xoff*xoff + yoff*yoff) > inner_radius)
        return "Patch offset out of bounds.";
    if ((aoff % 36) != 0 || aoff < 0 || aoff >= 360)
        return "Angle offset out of bounds.";

    return NULL;
}

/*
 * We're asked for a grid of a particular size, and we generate enough
 * of the tiling so we can be sure to have enough random grid from which
 * to pick.
 */

static grid *grid_new_penrose(int width, int height, int which, char *desc)
{
    int max_faces, max_dots, tilesize = PENROSE_TILESIZE;
    int xsz, ysz, xoff, yoff, aoff;
    double rradius;

    tree234 *points;
    grid *g;

    penrose_state ps;
    setface_ctx sf_ctx;

    penrose_calculate_size(which, tilesize, width, height,
                           &rradius, &ps.start_size, &ps.max_depth);

    debug(("penrose: w%d h%d, tile size %d, start size %d, depth %d",
           width, height, tilesize, ps.start_size, ps.max_depth));

    ps.new_tile = set_faces;
    ps.ctx = &sf_ctx;

    max_faces = (width*3) * (height*3); /* somewhat paranoid... */
    max_dots = max_faces * 4; /* ditto... */

    g = grid_empty();
    g->tilesize = tilesize;
    g->faces = snewn(max_faces, grid_face);
    g->dots = snewn(max_dots, grid_dot);

    points = newtree234(grid_point_cmp_fn);

    memset(&sf_ctx, 0, sizeof(sf_ctx));
    sf_ctx.g = g;
    sf_ctx.points = points;

    if (desc != NULL) {
        if (sscanf(desc, "G%d,%d,%d", &xoff, &yoff, &aoff) != 3)
            assert(!"Invalid grid description.");
    } else {
        xoff = yoff = 0;
    }

    xsz = width * tilesize;
    ysz = height * tilesize;

    sf_ctx.xmin = xoff - xsz/2;
    sf_ctx.xmax = xoff + xsz/2;
    sf_ctx.ymin = yoff - ysz/2;
    sf_ctx.ymax = yoff + ysz/2;

    debug(("penrose: centre (%f, %f) xsz %f ysz %f",
           0.0, 0.0, xsz, ysz));
    debug(("penrose: x range (%f --> %f), y range (%f --> %f)",
           sf_ctx.xmin, sf_ctx.xmax, sf_ctx.ymin, sf_ctx.ymax));

    penrose(&ps, which, aoff);

    freetree234(points);
    assert(g->num_faces <= max_faces);
    assert(g->num_dots <= max_dots);

    debug(("penrose: %d faces total (equivalent to %d wide by %d high)",
           g->num_faces, g->num_faces/height, g->num_faces/width));

    grid_trim_vigorously(g);
    grid_make_consistent(g);

    /*
     * Centre the grid in its originally promised rectangle.
     */
    g->lowest_x -= ((sf_ctx.xmax - sf_ctx.xmin) -
                    (g->highest_x - g->lowest_x)) / 2;
    g->highest_x = g->lowest_x + (sf_ctx.xmax - sf_ctx.xmin);
    g->lowest_y -= ((sf_ctx.ymax - sf_ctx.ymin) -
                    (g->highest_y - g->lowest_y)) / 2;
    g->highest_y = g->lowest_y + (sf_ctx.ymax - sf_ctx.ymin);

    return g;
}

static void grid_size_penrose_p2_kite(int width, int height,
                       int *tilesize, int *xextent, int *yextent)
{
    grid_size_penrose(width, height, tilesize, xextent, yextent);
}

static void grid_size_penrose_p3_thick(int width, int height,
                       int *tilesize, int *xextent, int *yextent)
{
    grid_size_penrose(width, height, tilesize, xextent, yextent);
}

static grid *grid_new_penrose_p2_kite(int width, int height, char *desc)
{
    return grid_new_penrose(width, height, PENROSE_P2, desc);
}

static grid *grid_new_penrose_p3_thick(int width, int height, char *desc)
{
    return grid_new_penrose(width, height, PENROSE_P3, desc);
}


/* Delaunay grids */

/* Choose a tilesize so that our integer coordinates are
 * small enough to survive cubing within 32 bits */
static int delaunay_tilesize(int width, int height)
{
    int m=width;
    if (height>m) m=height;
    return 200/m;
}

static void grid_size_delaunay(int width, int height,
                      int *tilesize, int *xextent, int *yextent)
{
    int a = delaunay_tilesize(width, height);

    *tilesize = a;
    *xextent = width * a;
    *yextent = height * a;
}

/* Check whether the pair of triangles meeting along the edge
 * satisfy the Delaunay criterion; if not, flip them. Return 1
 * if anything needed to be done.
 *
 * Needs a grid in which all edges are correctly linked to their faces
 * and vice versa. Does not update the lists of edges incident on dots.
 *
 */
static int delaunay_flip_edge(grid*g, grid_edge*e)
{
    int i;
    grid_dot *a, *b, *c, *d;
    grid_edge *ab, *bc, *cd, *da;

    /* Nothing to do if we border the infinite face */
    if (!e->face1 || !e->face2) return 0;

    a = e->dot1;
    c = e->dot2;
    for (i=0;i<3;i++) {
        b = e->face1->dots[i];
        if (b != a && b != c) break;
    }
    for (i=0;i<3;i++) {
        d = e->face2->dots[i];
        if (d != a && d != c) break;
    }
    assert (a!=b);
    assert (a!=c);
    assert (a!=d);
    assert (b!=c);
    assert (b!=d);
    assert (c!=d);

    /* Arrange ABCD anticlockwise and with AC as the edge */
    /* Also, B is on e->face1 */
    if (anticlockwise(a->x,a->y,
                      b->x,b->y,
                      c->x,c->y)<0) {
        grid_dot *t;
        grid_face *tf;
        t=b; b=d; d=t;
        tf=e->face1; e->face1=e->face2; e->face2=tf;
    }

    /* FIXME: make sure these really are in the right order */
    assert(anticlockwise(a->x,a->y,
                         b->x,b->y,
                         c->x,c->y)>0);
    assert(anticlockwise(a->x,a->y,
                         c->x,c->y,
                         d->x,d->y)>0);

    /* Does the edge need to be flipped? */
    if (!(in_circumcircle(a->x, a->y,
                          b->x, b->y,
                          c->x, c->y,
                          d->x, d->y) &&
          in_circumcircle(a->x, a->y,
                          c->x, c->y,
                          d->x, d->y,
                          b->x, b->y)))
        return 0;
    /* If flipping is needed, then ABCD must be convex */
    assert(anticlockwise(b->x,b->y,
                         c->x,c->y,
                         d->x,d->y)>0);
    assert(anticlockwise(c->x,c->y,
                         d->x,d->y,
                         a->x,a->y)>0);

    /* Make sure that the faces we're modifying are always face1 */
    for (i=0;i<6;i++) {
        grid_face *tf;
        grid_edge *u;

        if (i<3) u = e->face1->edges[i];
        else u = e->face2->edges[i-3];

        if (u!=e && (u->face2 == e->face1 || u->face2 == e->face2)) {
            tf = u->face1;
            u->face1 = u->face2;
            u->face2 = tf;
        }
    }

    /* Find named edges */
    ab = (grid_edge*) 0;
    bc = (grid_edge*) 0;
    cd = (grid_edge*) 0;
    da = (grid_edge*) 0;
    for (i=0;i<6;i++) {
        grid_edge *u;

        if (i<3) u = e->face1->edges[i];
        else u = e->face2->edges[i-3];

        if ((a==u->dot1 && b==u->dot2) ||
            (a==u->dot2 && b==u->dot1)) {
           ab = u;
        } else if ((b==u->dot1 && c==u->dot2) ||
            (b==u->dot2 && c==u->dot1)) {
           bc = u;
        } else if ((c==u->dot1 && d==u->dot2) ||
            (c==u->dot2 && d==u->dot1)) {
           cd = u;
        } else if ((d==u->dot1 && a==u->dot2) ||
            (d==u->dot2 && a==u->dot1)) {
           da = u;
        } else if ((a==u->dot1 && c==u->dot2) ||
            (a==u->dot2 && c==u->dot1)) {
           assert(u==e);
        } else {
           assert(0);
        }
    }
    assert(ab && bc && cd && da);

    assert(ab->face1 == e->face1);
    assert(bc->face1 == e->face1);
    assert(cd->face1 == e->face2);
    assert(da->face1 == e->face2);

    /* Now rearrange the faces so BD is the edge */
    e->dot1 = b;
    e->dot2 = d;

    /* Change which point goes where */
    e->face1->dots[0] = a;
    e->face1->dots[1] = b;
    e->face1->dots[2] = d;

    e->face2->dots[0] = b;
    e->face2->dots[1] = c;
    e->face2->dots[2] = d;

    /* Change which edge goes where */
    e->face1->edges[0] = ab;
    e->face1->edges[1] = e;
    e->face1->edges[2] = da;

    e->face2->edges[0] = bc;
    e->face2->edges[1] = cd;
    e->face2->edges[2] = e;

    /* Fix up which faces each edge borders */
    ab->face1 = e->face1;
    bc->face1 = e->face2;
    cd->face1 = e->face2;
    da->face1 = e->face1;

    return 1;
}

static grid *grid_new_delaunay(int width, int height, char *desc)
{
    int i, j;
    grid *g = grid_empty();
    grid *g1;
    tree234 *queue = newtree234(grid_edge_bydots_cmpfn);
    grid_face *oldfaces;
    int oldnumfaces;

    /* Side length */
    int a = delaunay_tilesize(width, height);

    /* Upper bounds - don't have to be exact */
    int max_dots = width * height + 14;
    int max_faces = 2*max_dots-6;
    int max_edges = 3*max_dots-7;

    assert(max_dots>3);

    g->tilesize = a;
    g->faces = snewn(max_faces+1, grid_face);
    g->dots = snewn(max_dots+1, grid_dot);
    g->edges = snewn(max_edges+1, grid_edge);

    /* Start with the outer rectangle; this is always a Delaunay
     * triangulation, and all later points will be within some
     * existing triangle
     */
    g->dots[0].x = 0;
    g->dots[0].y = 0;
    g->dots[1].x = a*width*1;
    g->dots[1].y = 0;
    g->dots[2].x = a*width*1;
    g->dots[2].y = a*height*1;
    g->dots[3].x = 0;
    g->dots[3].y = a*height*1;

    g->faces[0].order = 3;
    g->faces[0].dots = snewn(3,grid_dot*);
    g->faces[0].dots[0] = &g->dots[0];
    g->faces[0].dots[1] = &g->dots[1];
    g->faces[0].dots[2] = &g->dots[3];
    g->faces[1].order = 3;
    g->faces[1].dots = snewn(3,grid_dot*);
    g->faces[1].dots[0] = &g->dots[1];
    g->faces[1].dots[1] = &g->dots[2];
    g->faces[1].dots[2] = &g->dots[3];

    g->num_dots = 4;
    g->num_faces = 2;

    /* FIXME: convert to using grid data structures to allow easy navigation */
    for (i=1; g->num_dots<max_dots; i++) {
        int x = (int)(a*width*subrandom(2,i));
        int y = (int)(a*height*subrandom(3,i));

        for (j=0;j<g->num_faces;j++) {
            grid_face*f = &g->faces[j];
            if (in_triangle(f->dots[0]->x, f->dots[0]->y,
                            f->dots[1]->x, f->dots[1]->y,
                            f->dots[2]->x, f->dots[2]->y,
                            x, y)) {
                /* Found the triangle that contains the new dot */

                grid_dot *d = &g->dots[g->num_dots++];
                grid_face *f1 = &g->faces[g->num_faces++];
                grid_face *f2 = &g->faces[g->num_faces++];

                d->x = x;
                d->y = y;

                f1->order = 3;
                f2->order = 3;
                f1->dots = snewn(3,grid_dot*);
                f2->dots = snewn(3,grid_dot*);

                /* Add two new triangles */
                f1->dots[0] = f->dots[0];
                f1->dots[1] = f->dots[1];
                f1->dots[2] = d;

                f2->dots[0] = f->dots[0];
                f2->dots[1] = d;
                f2->dots[2] = f->dots[2];

                /* Replace the old triangle with a new one sharing two
                 * corners
                 */
                f->dots[0] = d;

                break;
            }
        }
        /* If no triangle contained the new dot, then it was
         * collinear with an existing segment, or something else
         * weird happened. So we simply reject this dot and don't
         * increment num_dots.
         */

    }
    /* Now we have a probably-not-Delaunay triangulation */

    assert(g->num_faces == max_faces);
    assert(g->num_dots == max_dots);

    grid_make_consistent(g);

    for (i=0;i<g->num_edges;i++)
        add234(queue, &g->edges[i]);
    while (1) {
        grid_edge *e = delpos234(queue,0);
        if (!e) break;
        if (delaunay_flip_edge(g,e)) {
            for (i=0;i<3;i++) {
                add234(queue, e->face1->edges[i]);
                add234(queue, e->face2->edges[i]);
            }
            del234(queue,e);
        }
    }
    freetree234(queue);

    /* Strip edge and dot structures so we can rerun grid_make_consistent */
    for (i=0; i<g->num_dots; i++) {
        sfree(g->dots[i].edges);
        g->dots[i].edges = NULL;
        sfree(g->dots[i].faces);
        g->dots[i].faces = NULL;
    }
    for (i=0; i<g->num_faces; i++) {
        sfree(g->faces[i].edges);
        g->faces[i].edges = NULL;
        g->faces[i].has_incentre = 0;
    }
    sfree(g->edges);
    g->edges = NULL;

    if (1) {
        /* Remove four corners */

        /* Remove all faces touching a corner */
        oldnumfaces = g->num_faces;
        oldfaces = g->faces;

        g->num_faces = 0;
        g->faces = snewn(oldnumfaces, grid_face);
        for (i=0; i<oldnumfaces; i++) {
            int zap = 0;
            for (j=0; j<oldfaces[i].order; j++) {
                if (oldfaces[i].dots[j] - g->dots < 4) {
                    zap = 1;
                }
            }
            if (zap) {
                sfree(oldfaces[i].dots);
                oldfaces[i].dots = NULL;
            } else {
                g->faces[g->num_faces] = oldfaces[i];
                g->num_faces++;
            }
        }
        sfree(oldfaces);

        /* Remove the corner dots */
        g->num_dots -= 4;
        for (i=0; i<g->num_dots; i++) {
            g->dots[i] = g->dots[i+4];
        }
        for (i=0; i<g->num_faces; i++) {
            for (j=0; j<g->faces[i].order; j++) {
                g->faces[i].dots[j] -= 4;
            }
        }
    }

    grid_make_consistent(g);
    g1 = grid_dual(g);
    /* grid_free(g); */
    /* g = grid_dual(g1); */
    /* grid_free(g); */
    return g1;
}

/* ----------- End of grid generators ------------- */

#define FNNEW(upper,lower) &grid_new_ ## lower,
#define FNSZ(upper,lower) &grid_size_ ## lower,

static grid *(*(grid_news[]))(int, int, char*) = { GRIDGEN_LIST(FNNEW) };
static void(*(grid_sizes[]))(int, int, int*, int*, int*) = { GRIDGEN_LIST(FNSZ) };

char *grid_new_desc(grid_type type, int width, int height, int dual, random_state *rs)
{
    if (type != GRID_PENROSE_P2 && type != GRID_PENROSE_P3)
        return NULL;

    return grid_new_desc_penrose(type, width, height, rs);
}

char *grid_validate_desc(grid_type type, int width, int height, int dual, char *desc)
{
    if (type != GRID_PENROSE_P2 && type != GRID_PENROSE_P3) {
        if (desc != NULL)
            return "Grid description strings not used with this grid type";
        return NULL;
    }

    return grid_validate_desc_penrose(type, width, height, desc);
}

grid *grid_new(grid_type type, int width, int height, int dual, char *desc)
{
    char *err = grid_validate_desc(type, width, height, dual, desc);
    if (err) assert(!"Invalid grid description.");

    if (!dual)
    {
        return grid_news[type](width, height, desc);
    }
    else
    {
        grid *temp;
        grid *g;

        temp = grid_news[type](width, height, desc);
        g = grid_dual(temp);
        grid_free(temp);
        return g;
    }
}

void grid_compute_size(grid_type type, int width, int height,
                       int *tilesize, int *xextent, int *yextent)
{
    grid_sizes[type](width, height, tilesize, xextent, yextent);
}

/* ----------- End of grid helpers ------------- */

/* vim: set shiftwidth=4 tabstop=8: */
