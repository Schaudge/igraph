/* -*- mode: C -*-  */
/*
   IGraph library.
   Copyright (C) 2007-2012  Gabor Csardi <csardi.gabor@gmail.com>
   334 Harvard street, Cambridge, MA 02139 USA

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301 USA

*/

#include "igraph_games.h"

#include "igraph_constructors.h"
#include "igraph_dqueue.h"
#include "igraph_interface.h"
#include "igraph_memory.h"
#include "igraph_random.h"
#include "igraph_progress.h"

#include "core/interruption.h"

typedef struct igraph_i_forest_fire_data_t {
    igraph_vector_int_t *inneis;
    igraph_vector_int_t *outneis;
    igraph_integer_t no_of_nodes;
} igraph_i_forest_fire_data_t;


static void igraph_i_forest_fire_free(igraph_i_forest_fire_data_t *data) {
    for (igraph_integer_t i = 0; i < data->no_of_nodes; i++) {
        igraph_vector_int_destroy(data->inneis + i);
        igraph_vector_int_destroy(data->outneis + i);
    }
}

/**
 * \function igraph_forest_fire_game
 * \brief Generates a network according to the \quote forest fire game \endquote.
 *
 * The forest fire model intends to reproduce the following network
 * characteristics, observed in real networks:
 * \ilist
 * \ili Heavy-tailed in- and out-degree distributions.
 * \ili Community structure.
 * \ili Densification power-law. The network is densifying in time,
 *      according to a power-law rule.
 * \ili Shrinking diameter. The diameter of the network decreases in
 *      time.
 * \endilist
 *
 * </para><para>
 * The network is generated in the following way. One vertex is added at
 * a time. This vertex connects to (cites) <code>ambs</code> vertices already
 * present in the network, chosen uniformly random. Now, for each cited
 * vertex <code>v</code> we do the following procedure:
 * \olist
 * \oli We generate two random numbers, <code>x</code> and <code>y</code>, that are
 *   geometrically distributed with means <code>p/(1-p)</code> and
 *   <code>rp(1-rp)</code>. (<code>p</code> is \p fw_prob, <code>r</code> is
 *   \p bw_factor.) The new vertex cites <code>x</code> outgoing neighbors
 *   and <code>y</code> incoming neighbors of <code>v</code>, from those which are
 *   not yet cited by the new vertex. If there are less than <code>x</code> or
 *   <code>y</code> such vertices available then we cite all of them.
 * \oli The same procedure is applied to all the newly cited
 *   vertices.
 * \endolist
 *
 * </para><para>
 * See also:
 * Jure Leskovec, Jon Kleinberg and Christos Faloutsos. Graphs over time:
 * densification laws, shrinking diameters and possible explanations.
 * \emb KDD '05: Proceeding of the eleventh ACM SIGKDD international
 * conference on Knowledge discovery in data mining \eme, 177--187, 2005.
 *
 * </para><para>
 * Note however, that the version of the model in the published paper is incorrect
 * in the sense that it cannot generate the kind of graphs the authors
 * claim. A corrected version is available from
 * https://www.cs.cmu.edu/~jure/pubs/powergrowth-tkdd.pdf, our
 * implementation is based on this.
 *
 * \param graph Pointer to an uninitialized graph object.
 * \param nodes The number of vertices in the graph.
 * \param fw_prob The forward burning probability.
 * \param bw_factor The backward burning ratio. The backward burning
      probability is calculated as <code>bw_factor * fw_prob</code>.
 * \param pambs The number of ambassador vertices.
 * \param directed Whether to create a directed graph.
 * \return Error code.
 *
 * Time complexity: TODO.
 */

igraph_error_t igraph_forest_fire_game(igraph_t *graph, igraph_integer_t nodes,
                            igraph_real_t fw_prob, igraph_real_t bw_factor,
                            igraph_integer_t pambs, igraph_bool_t directed) {

    igraph_vector_int_t visited;
    igraph_integer_t no_of_nodes = nodes, actnode, i;
    igraph_vector_int_t edges;
    igraph_vector_int_t *inneis, *outneis;
    igraph_i_forest_fire_data_t data;
    igraph_dqueue_int_t neiq;
    igraph_integer_t ambs = pambs;
    igraph_real_t param_geom_out = 1 - fw_prob;
    igraph_real_t param_geom_in = 1 - fw_prob * bw_factor;

    if (fw_prob < 0 || fw_prob >= 1) {
        IGRAPH_ERROR("Forest fire model: 'fw_prob' must satisfy 0 <= fw_prob < 1.",
                     IGRAPH_EINVAL);
    }
    if (bw_factor * fw_prob < 0 || bw_factor * fw_prob >= 1) {
        IGRAPH_ERROR("Forest fire model: 'bw_factor' must satisfy 0 <= bw_factor * fw_prob < 1.",
                     IGRAPH_EINVAL);
    }
    if (ambs < 0) {
        IGRAPH_ERROR("Forest fire model: Number of ambassadors must not be negative.",
                     IGRAPH_EINVAL);
    }

    if (ambs == 0) {
        IGRAPH_CHECK(igraph_empty(graph, nodes, directed));
        return IGRAPH_SUCCESS;
    }

    IGRAPH_VECTOR_INT_INIT_FINALLY(&edges, 0);

    inneis = IGRAPH_CALLOC(no_of_nodes, igraph_vector_int_t);
    IGRAPH_CHECK_OOM(inneis, "Insufficient memory for forest fire model.");
    IGRAPH_FINALLY(igraph_free, inneis);

    outneis = IGRAPH_CALLOC(no_of_nodes, igraph_vector_int_t);
    IGRAPH_CHECK_OOM(outneis, "Insufficient memory for forest fire model.");
    IGRAPH_FINALLY(igraph_free, outneis);

    data.inneis = inneis;
    data.outneis = outneis;
    data.no_of_nodes = no_of_nodes;
    IGRAPH_FINALLY(igraph_i_forest_fire_free, &data);
    for (i = 0; i < no_of_nodes; i++) {
        IGRAPH_CHECK(igraph_vector_int_init(inneis + i, 0));
        IGRAPH_CHECK(igraph_vector_int_init(outneis + i, 0));
    }

    IGRAPH_CHECK(igraph_vector_int_init(&visited, no_of_nodes));
    IGRAPH_FINALLY(igraph_vector_int_destroy, &visited);
    IGRAPH_DQUEUE_INT_INIT_FINALLY(&neiq, 10);

    RNG_BEGIN();

#define ADD_EDGE_TO(nei) \
    if (VECTOR(visited)[(nei)] != actnode+1) { \
        VECTOR(visited)[(nei)] = actnode+1; \
        IGRAPH_CHECK(igraph_dqueue_int_push(&neiq, (nei))); \
        IGRAPH_CHECK(igraph_vector_int_push_back(&edges, actnode)); \
        IGRAPH_CHECK(igraph_vector_int_push_back(&edges, (nei))); \
        IGRAPH_CHECK(igraph_vector_int_push_back(outneis+actnode, (nei))); \
        IGRAPH_CHECK(igraph_vector_int_push_back(inneis+(nei), actnode)); \
    }

    IGRAPH_PROGRESS("Forest fire: ", 0.0, NULL);

    for (actnode = 1; actnode < no_of_nodes; actnode++) {

        IGRAPH_PROGRESS("Forest fire: ", 100.0 * actnode / no_of_nodes, NULL);

        IGRAPH_ALLOW_INTERRUPTION();

        /* We don't want to visit the current vertex */
        VECTOR(visited)[actnode] = actnode + 1;

        /* Choose ambassador(s) */
        for (i = 0; i < ambs; i++) {
            igraph_integer_t a = RNG_INTEGER(0, actnode - 1);
            ADD_EDGE_TO(a);
        }

        while (!igraph_dqueue_int_empty(&neiq)) {
            igraph_integer_t actamb = igraph_dqueue_int_pop(&neiq);
            igraph_vector_int_t *outv = outneis + actamb;
            igraph_vector_int_t *inv = inneis + actamb;
            igraph_integer_t no_in = igraph_vector_int_size(inv);
            igraph_integer_t no_out = igraph_vector_int_size(outv);
            igraph_integer_t neis_out = RNG_GEOM(param_geom_out);
            igraph_integer_t neis_in = RNG_GEOM(param_geom_in);
            /* outgoing neighbors */
            if (neis_out >= no_out) {
                for (i = 0; i < no_out; i++) {
                    igraph_integer_t nei = VECTOR(*outv)[i];
                    ADD_EDGE_TO(nei);
                }
            } else {
                igraph_integer_t oleft = no_out;
                for (i = 0; i < neis_out && oleft > 0; ) {
                    igraph_integer_t which = RNG_INTEGER(0, oleft - 1);
                    igraph_integer_t nei = VECTOR(*outv)[which];
                    VECTOR(*outv)[which] = VECTOR(*outv)[oleft - 1];
                    VECTOR(*outv)[oleft - 1] = nei;
                    if (VECTOR(visited)[nei] != actnode + 1) {
                        ADD_EDGE_TO(nei);
                        i++;
                    }
                    oleft--;
                }
            }
            /* incoming neighbors */
            if (neis_in >= no_in) {
                for (i = 0; i < no_in; i++) {
                    igraph_integer_t nei = VECTOR(*inv)[i];
                    ADD_EDGE_TO(nei);
                }
            } else {
                igraph_integer_t ileft = no_in;
                for (i = 0; i < neis_in && ileft > 0; ) {
                    igraph_integer_t which = RNG_INTEGER(0, ileft - 1);
                    igraph_integer_t nei = VECTOR(*inv)[which];
                    VECTOR(*inv)[which] = VECTOR(*inv)[ileft - 1];
                    VECTOR(*inv)[ileft - 1] = nei;
                    if (VECTOR(visited)[nei] != actnode + 1) {
                        ADD_EDGE_TO(nei);
                        i++;
                    }
                    ileft--;
                }
            }

        } /* while neiq not empty */

    } /* actnode < no_of_nodes */

#undef ADD_EDGE_TO

    RNG_END();

    IGRAPH_PROGRESS("Forest fire: ", 100.0, NULL);

    igraph_dqueue_int_destroy(&neiq);
    igraph_vector_int_destroy(&visited);
    igraph_i_forest_fire_free(&data);
    igraph_free(outneis);
    igraph_free(inneis);
    IGRAPH_FINALLY_CLEAN(5);

    IGRAPH_CHECK(igraph_create(graph, &edges, nodes, directed));
    igraph_vector_int_destroy(&edges);
    IGRAPH_FINALLY_CLEAN(1);

    return IGRAPH_SUCCESS;
}
