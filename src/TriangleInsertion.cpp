//
// Created by Yixin Hu on 2019-08-27.
//

#include <floattetwild/TriangleInsertion.h>

#include <floattetwild/FloatTetCutting.h>
#include <floattetwild/FloatTetCuttingCheck.h>
#include <floattetwild/FloatTetCuttingParallel.h>
#include <floattetwild/LocalOperations.h>
#include <floattetwild/Predicates.hpp>
#include <floattetwild/MeshIO.hpp>
#include <floattetwild/auto_table.hpp>
#include <floattetwild/Logger.hpp>
#include <floattetwild/intersections.h>


#include <igl/writeSTL.h>
#include <igl/Timer.h>

#ifdef FLOAT_TETWILD_USE_TBB
#include <tbb/task_scheduler_init.h>
#include <tbb/parallel_for.h>
#include <tbb/atomic.h>
#include <floattetwild/FloatTetCuttingParallel.h>
#include <tbb/concurrent_queue.h>
#endif

#include <bitset>
#include <numeric>
#include <unordered_map>

void floatTetWild::insert_triangles(const std::vector<Vector3> &input_vertices,
        const std::vector<Vector3i> &input_faces, const std::vector<int> &input_tags,
        Mesh &mesh, std::vector<bool> &is_face_inserted, AABBWrapper &tree, bool is_again) {
    //todo: mark inserted faces on mesh before calling this function!!!

    for (int i = 0; i < input_faces.size(); i++) {
        if (is_face_inserted[i])
            continue;
        insert_one_triangle(i, input_vertices, input_faces, input_tags, mesh, is_face_inserted, tree, is_again);
    }
}

bool floatTetWild::insert_one_triangle(int insert_f_id, const std::vector<Vector3> &input_vertices,
        const std::vector<Vector3i> &input_faces, const std::vector<int> &input_tags,
        Mesh &mesh, std::vector<bool> &is_face_inserted, AABBWrapper &tree, bool is_again) {

    std::array<Vector3, 3> vs = {{input_vertices[input_faces[insert_f_id][0]],
                                         input_vertices[input_faces[insert_f_id][1]],
                                         input_vertices[input_faces[insert_f_id][2]]}};
    Vector3 n = (vs[1] - vs[0]).cross(vs[2] - vs[0]);
    int t = get_t(vs[0], vs[1], vs[2]);

    /////
    std::vector<int> cut_t_ids;
    //todo: find tet for cutting, BFS!!


    /////
    CutMesh cut_mesh(mesh, n, vs);
    cut_mesh.construct(cut_t_ids);
    if(cut_mesh.snap_to_plane())
        cut_mesh.expand(cut_t_ids);

    /////
    std::map<std::array<int, 2>, Vector3> map_edge_to_intersecting_point;
    std::vector<int> subdivide_t_ids;
    if(cut_mesh.get_intersecting_edges_and_points(map_edge_to_intersecting_point, subdivide_t_ids) == false)
        return false;

    /////
    //todo: mark "coplanar" tet faces!!!

    for(int t_id: subdivide_t_ids){

    }

    return true;
}

void floatTetWild::CutMesh::construct(const std::vector<int>& cut_t_ids) {
    v_ids.reserve(cut_t_ids.size() * 4);
    for (int t_id:cut_t_ids) {
        for (int j = 0; j < 4; j++)
            v_ids.push_back(mesh.tets[t_id][j]);
    }
    vector_unique(v_ids);

    for (int i = 0; i < v_ids.size(); i++)
        map_v_ids[v_ids[i]] = i;

    tets.resize(cut_t_ids.size());
    for (int i = 0; i < cut_t_ids.size(); i++) {
        tets[i] = {{map_v_ids[mesh.tets[cut_t_ids[i]][0]],
                           map_v_ids[mesh.tets[cut_t_ids[i]][1]],
                           map_v_ids[mesh.tets[cut_t_ids[i]][2]]}};
    }

    std::vector<std::vector<int>> conn_tets(v_ids.size());
    for (int i = 0; i < tets.size(); i++) {
        for (int j = 0; j < 4; j++)
            conn_tets[tets[i][j]].push_back(i);
    }

    for (int i = 0; i < tets.size(); i++) {
        for (int j = 0; j < 4; j++) {
            if (opp_t_ids[i][j] >= 0)
                continue;
            std::vector<int> n_t_ids;
            set_intersection(conn_tets[tets[i][(j + 1) % 4]], conn_tets[tets[i][(j + 2) % 4]],
                             conn_tets[tets[i][(j + 3) % 4]], n_t_ids);
            assert(!n_t_ids.empty());
            if (n_t_ids.size() < 2)
                continue;

            int n_t_id = n_t_ids[0] == i ? n_t_ids[1] : n_t_ids[0];
            opp_t_ids[i][j] = n_t_id;
            for (int k = 0; k < 4; k++) {
                if (tets[n_t_id][k] != tets[i][(j + 1) % 4] && tets[n_t_id][k] != tets[i][(j + 2) % 4]
                    && tets[n_t_id][k] != tets[i][(j + 3) % 4]) {
                    opp_t_ids[n_t_id][k] = i;
                    break;
                }
            }
        }
    }


}

bool floatTetWild::CutMesh::snap_to_plane() {
    bool snapped = false;
    to_plane_dists.resize(map_v_ids.size());
    for (auto &v:map_v_ids) {
        int v_id = v.first;
        int lv_id = v.second;

        int ori = Predicates::orient_3d(p_vs[0], p_vs[1], p_vs[2], mesh.tet_vertices[v_id].pos);
        if (ori == Predicates::ORI_ZERO) {
            to_plane_dists[lv_id] = 0;
            continue;
        }
        to_plane_dists[lv_id] = get_to_plane_dist(mesh.tet_vertices[lv_id].pos);
        if (ori == Predicates::ORI_POSITIVE && to_plane_dists[lv_id] < 0
            || ori == Predicates::ORI_NEGATIVE && to_plane_dists[lv_id] > 0)
            to_plane_dists[lv_id] = -to_plane_dists[lv_id];

        if (std::abs(to_plane_dists[lv_id]) < mesh.params.eps_2_coplanar) {
            is_snapped[lv_id] = true;
            snapped = true;
        }
    }
    for (auto &t: tets) {
        if (is_v_on_plane(t[0]) && is_v_on_plane(t[1]) && is_v_on_plane(t[2]) && is_v_on_plane(t[3])) {
            auto tmp_t = t;
            std::sort(tmp_t.begin(), tmp_t.end(), [&](int a, int b) {
                return to_plane_dists[a] < to_plane_dists[b];
            });
            for (int j = 3; j >= 0; j--) {
                if (is_snapped[tmp_t.back()] == true)
                    is_snapped[tmp_t.back()] = false;
            }
        }
    }

    return snapped;
}

void floatTetWild::CutMesh::expand(std::vector<int>& cut_t_ids) {
    while (true) {
        std::vector<std::array<int, 5>> new_opp_t_ids;
        for (int i = 0; i < opp_t_ids.size(); i++) {
            for (int j = 0; j < 4; j++) {
                if (opp_t_ids[i][j] >= 0)
                    continue;
                if (!is_snapped[tets[i][(j + 1) % 4]] && !is_snapped[tets[i][(j + 2) % 4]]
                    && !is_snapped[tets[i][(j + 3) % 4]])
                    continue;
                std::vector<int> n_t_ids;
                set_intersection(mesh.tet_vertices[v_ids[tets[i][(j + 1) % 4]]].conn_tets,
                                 mesh.tet_vertices[v_ids[tets[i][(j + 2) % 4]]].conn_tets,
                                 mesh.tet_vertices[v_ids[tets[i][(j + 3) % 4]]].conn_tets,
                                 n_t_ids);
                if (n_t_ids.size() == 1)
                    continue;
                int new_t_id = n_t_ids[0] == cut_t_ids[i] ? n_t_ids[1] : n_t_ids[0];
                new_opp_t_ids.push_back({{-1, -1, -1, -1, new_t_id}});
                for (int j = 0; j < 4; j++) {
                    if (mesh.tets[new_t_id][j] != v_ids[tets[i][(j + 1) % 4]]
                        && mesh.tets[new_t_id][j] != v_ids[tets[i][(j + 2) % 4]]
                        && mesh.tets[new_t_id][j] != v_ids[tets[i][(j + 3) % 4]]) {
                        new_opp_t_ids.back()[j] = i;
                        break;
                    }
                }
            }
        }
        std::sort(new_opp_t_ids.begin(), new_opp_t_ids.end(),
                  [&](const std::array<int, 5> &a, const std::array<int, 5> &b) {
                      return a.back() < b.back();
                  });
        for (int i = 0; i < new_opp_t_ids.size() - 1; i++) {
            if (new_opp_t_ids[i].back() == new_opp_t_ids[i + 1].back()) {
                for (int j = 0; j < 4; j++) {
                    if (new_opp_t_ids[i][j] >= 0) {
                        assert(new_opp_t_ids[i + 1][j] < 0);
                        new_opp_t_ids[i + 1][j] = new_opp_t_ids[i][j];
                        break;
                    }
                }
                new_opp_t_ids.erase(new_opp_t_ids.begin() + i);
                i--;
            }
        }

        //todo: update CutMesh, topo & snapping
        bool snapped = false;
        const int old_tets_size = tets.size();
        for (int i = 0; i < new_opp_t_ids.size(); i++) {
            ///
            int cnt_pos = 0;
            int cnt_neg = 0;
            for (int j = 0; j < 4; j++) {
                int ori = Predicates::orient_3d(p_vs[0], p_vs[1], p_vs[2],
                                                mesh.tet_vertices[mesh.tets[new_opp_t_ids[i].back()][j]].pos);
                if (ori == Predicates::ORI_POSITIVE)
                    cnt_pos++;
                else if (ori == Predicates::ORI_NEGATIVE)
                    cnt_neg++;
            }
            if (cnt_neg == 0 || cnt_pos == 0)
                continue;

            ///
            cut_t_ids.push_back(new_opp_t_ids[i].back());

            int t_id = tets.size();
            tets.emplace_back();
            auto &t = tets.back();
            for (int j = 0; j < 4; j++) {
                int v_id = mesh.tets[new_opp_t_ids[i].back()][j];
                if (map_v_ids.find(v_id) == map_v_ids.end()) {
                    v_ids.push_back(v_id);
                    int lv_id = v_ids.size() - 1;
                    map_v_ids[v_id] = lv_id;
                    to_plane_dists[lv_id] = get_to_plane_dist(mesh.tet_vertices[lv_id].pos);
                    if (std::abs(to_plane_dists[lv_id]) < mesh.params.eps_2_coplanar) {
                        is_snapped[lv_id] = true;
                        snapped = true;
                    }
                }
                t[j] = map_v_ids[v_id];
            }

            opp_t_ids.emplace_back();
            auto &opp = opp_t_ids.back();
            for (int j = 0; j < 4; j++) {
                opp[j] = new_opp_t_ids[i][j];
                if (new_opp_t_ids[i][j] < 0)
                    continue;
                int opp_t_id = new_opp_t_ids[i][j];
                for (int k = 0; k < 4; k++) {
                    if (tets[opp_t_id][k] != t[(j + 1) % 4] && tets[opp_t_id][k] != t[(j + 2) % 4]
                        && tets[opp_t_id][k] != t[(j + 3) % 4]) {
                        opp_t_ids[opp_t_id][k] = t_id;
                        break;
                    }
                }
            }
        }
        if (old_tets_size == tets.size())
            break;
        if (!snapped)
            break;

        for (int i = old_tets_size; i < tets.size(); i++) {
            const auto &t = tets[i];
            if (is_v_on_plane(t[0]) && is_v_on_plane(t[1]) && is_v_on_plane(t[2]) && is_v_on_plane(t[3])) {
                auto tmp_t = t;
                std::sort(tmp_t.begin(), tmp_t.end(), [&](int a, int b) {
                    return to_plane_dists[a] < to_plane_dists[b];
                });
                for (int j = 3; j >= 0; j--) {
                    if (is_snapped[tmp_t.back()] == true)
                        is_snapped[tmp_t.back()] = false;
                }
            }
        }
    }
}

bool floatTetWild::CutMesh::get_intersecting_edges_and_points(std::map<std::array<int, 2>, Vector3>& map_edge_to_intersecting_point,
                                                              std::vector<int>& subdivide_t_ids) {
    std::vector<std::array<int, 2>> edges;
    for (auto &t: tets) {
        for (int j = 0; j < 3; j++) {
            std::array<int, 2> e;
            if (e[0] > e[1])
                e = {{t[0], t[j + 1]}};
            else
                e = {{t[j + 1], t[0]}};
            edges.push_back(e);
            e = {{t[j + 1], t[mod3(j + 1) + 1]}};
            if (e[0] > e[1])
                std::swap(e[0], e[1]);
            edges.push_back(e);
        }
    }
    vector_unique(edges);

    std::vector<bool> is_intersected(edges.size(), false);
    for (int i = 0; i < edges.size(); i++) {
        auto &e = edges[i];
        if (is_v_on_plane(e[0]) || is_v_on_plane(e[1]))
            continue;
        if (to_plane_dists[e[0]] > 0 && to_plane_dists[e[1]] > 0
            || to_plane_dists[e[0]] < 0 && to_plane_dists[e[1]] < 0)
            continue;

        Vector3 p;
        Scalar _;
        bool is_result = seg_plane_intersection(mesh.tet_vertices[v_ids[e[0]]].pos, mesh.tet_vertices[v_ids[e[1]]].pos,
                                                p_vs[0], p_n, p, _);
        if (!is_result) {
            return false;
        }
        map_edge_to_intersecting_point[e] = p;
    }


    for (int i = 0; i < edges.size(); i++) {
        if (!is_intersected[i])
            continue;
        auto &e = edges[i];
        std::vector<int> tmp;
        set_intersection(mesh.tet_vertices[v_ids[e[0]]].conn_tets, mesh.tet_vertices[v_ids[e[1]]].conn_tets, tmp);
        subdivide_t_ids.insert(subdivide_t_ids.end(), tmp.begin(), tmp.end());
    }
    vector_unique(subdivide_t_ids);

    return true;
}