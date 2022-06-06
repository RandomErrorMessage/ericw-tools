/*
    Copyright (C) 1996-1997  Id Software, Inc.
    Copyright (C) 1997       Greg Lewis

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
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/
// merge.c

#include <qbsp/merge.hh>

#include <qbsp/qbsp.hh>
#include <qbsp/csg4.hh>
#include <qbsp/map.hh>
#include <qbsp/surfaces.hh>

#ifdef PARANOID
static void CheckColinear(face_t *f)
{
    int i, j;
    qvec3d v1, v2;

    for (i = 0; i < f->w.numpoints; i++) {
        // skip the point if the vector from the previous point is the same
        // as the vector to the next point
        j = (i - 1 < 0) ? f->w.numpoints - 1 : i - 1;
        v1 = qv::normalize(f->w.points[i] - f->w.points[j]);

        j = (i + 1 == f->w.numpoints) ? 0 : i + 1;
        v2 = qv::normalize(f->w.points[j] - f->w.points[i]);

        if (qv::epsilonEqual(v1, v2, EQUAL_EPSILON))
            FError("Colinear edge");
    }
}
#endif /* PARANOID */

/*
=============
TryMerge

If two polygons share a common edge and the edges that meet at the
common points are both inside the other polygons, merge them

Returns NULL if the faces couldn't be merged, or the new face.
The originals will NOT be freed.
=============
*/
static face_t *TryMerge(face_t *f1, face_t *f2)
{
    qvec3d p1, p2, p3, p4, back;
    face_t *newf;
    int i, j, k, l;
    qvec3d normal, delta, planenormal;
    vec_t dot;
    bool keep1, keep2;

    if (!f1->w.size() || !f2->w.size() || f1->planeside != f2->planeside || f1->texinfo != f2->texinfo ||
        f1->contents != f2->contents || f1->lmshift != f2->lmshift)
        return NULL;

    // find a common edge
    p1 = p2 = NULL; // stop compiler warning
    j = 0; //

    for (i = 0; i < f1->w.size(); i++) {
        p1 = f1->w[i];
        p2 = f1->w[(i + 1) % f1->w.size()];
        for (j = 0; j < f2->w.size(); j++) {
            p3 = f2->w[j];
            p4 = f2->w[(j + 1) % f2->w.size()];
            for (k = 0; k < 3; k++) {
                if (fabs(p1[k] - p4[k]) > EQUAL_EPSILON || fabs(p2[k] - p3[k]) > EQUAL_EPSILON)
                    break;
            }
            if (k == 3)
                break;
        }
        if (j < f2->w.size())
            break;
    }

    if (i == f1->w.size())
        return NULL; // no matching edges

    // check slope of connected lines
    // if the slopes are colinear, the point can be removed
    const auto &plane = map.planes.at(f1->planenum);
    planenormal = plane.normal;
    if (f1->planeside)
        planenormal = -planenormal;

    back = f1->w[(i + f1->w.size() - 1) % f1->w.size()];
    delta = p1 - back;
    normal = qv::normalize(qv::cross(planenormal, delta));

    back = f2->w[(j + 2) % f2->w.size()];
    delta = back - p1;
    dot = qv::dot(delta, normal);
    if (dot > CONTINUOUS_EPSILON)
        return NULL; // not a convex polygon
    keep1 = dot < -CONTINUOUS_EPSILON;

    back = f1->w[(i + 2) % f1->w.size()];
    delta = back - p2;
    normal = qv::normalize(qv::cross(planenormal, delta));

    back = f2->w[(j + f2->w.size() - 1) % f2->w.size()];
    delta = back - p2;
    dot = qv::dot(delta, normal);
    if (dot > CONTINUOUS_EPSILON)
        return NULL; // not a convex polygon
    keep2 = dot < -CONTINUOUS_EPSILON;

    // build the new polygon
    if (f1->w.size() + f2->w.size() > MAXEDGES) {
        logging::funcprint("WARNING: Too many edges\n");
        return NULL;
    }

    newf = NewFaceFromFace(f1);

    // copy first polygon
    if (keep2)
        k = (i + 1) % f1->w.size();
    else
        k = (i + 2) % f1->w.size();
    for (; k != i; k = (k + 1) % f1->w.size()) {
        newf->w.push_back(f1->w[k]);
    }

    // copy second polygon
    if (keep1)
        l = (j + 1) % f2->w.size();
    else
        l = (j + 2) % f2->w.size();
    for (; l != j; l = (l + 1) % f2->w.size()) {
        newf->w.push_back(f2->w[l]);
    }

    UpdateFaceSphere(newf);

    return newf;
}

/*
===============
MergeFaceToList
===============
*/
void MergeFaceToList(face_t *face, std::list<face_t *> &list)
{
    for (auto it = list.begin(); it != list.end();) {
#ifdef PARANOID
        CheckColinear(f);
#endif
        face_t *newf = TryMerge(face, *it);

        if (newf) {
            delete face;
            delete *it;
            list.erase(it);
            // restart, now trying to merge `newf` into the list
            face = newf;
            it = list.begin();
        } else {
            it++;
        }
    }

    list.emplace_back(face);
}

/*
===============
MergeFaceList
===============
*/
inline std::list<face_t *> MergeFaceList(std::list<face_t *> input)
{
    std::list<face_t *> result;

    for (face_t * face : input) {
        MergeFaceToList(face, result);
    }

    return result;
}

#include <tbb/parallel_for_each.h>

static void CollectNodes_R(node_t *node, std::vector<node_t *> &allnodes)
{
    allnodes.push_back(node);

    if (node->planenum == PLANENUM_LEAF) {
        return;
    }

    CollectNodes_R(node->children[0], allnodes);
    CollectNodes_R(node->children[1], allnodes);
}

/*
============
MergeAll
============
*/
void MergeAll(node_t *headnode)
{
    std::atomic<int> mergefaces = 0, premergefaces = 0;

    logging::print(logging::flag::PROGRESS, "---- {} ----\n", __func__);

    std::vector<node_t *> allnodes;
    CollectNodes_R(headnode, allnodes);

    tbb::parallel_for_each(allnodes, [&](node_t *node) {
        const size_t before = node->facelist.size();

        std::list<face_t *> merged_pre_subdivide = MergeFaceList(node->facelist);

        // re-subdivide after merging
        node->facelist.clear();
        for (face_t *face : merged_pre_subdivide) {
            node->facelist.splice(node->facelist.end(), SubdivideFace(face));
        }

        const size_t after = node->facelist.size();

        premergefaces += before;
        mergefaces += after;
    });

    logging::print(logging::flag::STAT, "     {:8} mergefaces (from {}; {:.0}% merged)\n", mergefaces, premergefaces,
        (static_cast<double>(mergefaces) / premergefaces) * 100.);
}
