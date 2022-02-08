#ifndef SLASUPPORTTREEALGORITHM_H
#define SLASUPPORTTREEALGORITHM_H

#include <cstdint>
#include <optional>

#include <libslic3r/SLA/SupportTreeBuilder.hpp>
#include <libslic3r/SLA/Clustering.hpp>
#include <libslic3r/SLA/SpatIndex.hpp>

namespace Slic3r {
namespace sla {

// The minimum distance for two support points to remain valid.
const double /*constexpr*/ D_SP = 0.1;

template <class T>
std::pair<T, T> dir_to_spheric(const Vec<3, T> &n, T norm = 1.)
{
    T z       = n.z();
    T r       = norm;
    T polar   = std::acos(z / r);
    T azimuth = std::atan2(n(1), n(0));
    return {polar, azimuth};
}

template <class T = double>
Vec<3, T> spheric_to_dir(double polar, double azimuth)
{
    return {T(std::cos(azimuth) * std::sin(polar)),
            T(std::sin(azimuth) * std::sin(polar)), T(std::cos(polar))};
}

template <class T = double, class Pair>
Vec<3, T> spheric_to_dir(const Pair &v)
{
    double plr = std::get<0>(v), azm = std::get<1>(v);
    return spheric_to_dir<T>(plr, azm);
}

// Give points on a 3D ring with given center, radius and orientation
// method based on:
// https://math.stackexchange.com/questions/73237/parametric-equation-of-a-circle-in-3d-space
template<size_t N>
class PointRing {
    std::array<double, N> m_phis;

    // Two vectors that will be perpendicular to each other and to the
    // axis. Values for a(X) and a(Y) are now arbitrary, a(Z) is just a
    // placeholder.
    // a and b vectors are perpendicular to the ring direction and to each other.
    // Together they define the plane where we have to iterate with the
    // given angles in the 'm_phis' vector
    Vec3d a = {0, 1, 0}, b;
    double m_radius = 0.;

    static inline bool constexpr is_one(double val)
    {
        return std::abs(std::abs(val) - 1) < 1e-20;
    }

public:

    PointRing(const Vec3d &n)
    {
        m_phis = linspace_array<N>(0., 2 * PI);

        // We have to address the case when the direction vector v (same as
        // dir) is coincident with one of the world axes. In this case two of
        // its components will be completely zero and one is 1.0. Our method
        // becomes dangerous here due to division with zero. Instead, vector
        // 'a' can be an element-wise rotated version of 'v'
        if(is_one(n(X)) || is_one(n(Y)) || is_one(n(Z))) {
            a = {n(Z), n(X), n(Y)};
            b = {n(Y), n(Z), n(X)};
        }
        else {
            a(Z) = -(n(Y)*a(Y)) / n(Z); a.normalize();
            b = a.cross(n);
        }
    }

    Vec3d get(size_t idx, const Vec3d src, double r) const
    {
        double phi = m_phis[idx];
        double sinphi = std::sin(phi);
        double cosphi = std::cos(phi);

        double rpscos = r * cosphi;
        double rpssin = r * sinphi;

        // Point on the sphere
        return {src(X) + rpscos * a(X) + rpssin * b(X),
                src(Y) + rpscos * a(Y) + rpssin * b(Y),
                src(Z) + rpscos * a(Z) + rpssin * b(Z)};
    }
};

//IndexedMesh::hit_result query_hit(const SupportableMesh &msh, const Bridge &br, double safety_d = std::nan(""));
//IndexedMesh::hit_result query_hit(const SupportableMesh &msh, const Head &br, double safety_d = std::nan(""));

inline Vec3d dirv(const Vec3d& startp, const Vec3d& endp) {
    return (endp - startp).normalized();
}

class PillarIndex {
    PointIndex m_index;
    using Mutex = ccr::BlockingMutex;
    mutable Mutex m_mutex;

public:

    template<class...Args> inline void guarded_insert(Args&&...args)
    {
        std::lock_guard<Mutex> lck(m_mutex);
        m_index.insert(std::forward<Args>(args)...);
    }

    template<class...Args>
    inline std::vector<PointIndexEl> guarded_query(Args&&...args) const
    {
        std::lock_guard<Mutex> lck(m_mutex);
        return m_index.query(std::forward<Args>(args)...);
    }

    template<class...Args> inline void insert(Args&&...args)
    {
        m_index.insert(std::forward<Args>(args)...);
    }

    template<class...Args>
    inline std::vector<PointIndexEl> query(Args&&...args) const
    {
        return m_index.query(std::forward<Args>(args)...);
    }

    template<class Fn> inline void foreach(Fn fn) { m_index.foreach(fn); }
    template<class Fn> inline void guarded_foreach(Fn fn)
    {
        std::lock_guard<Mutex> lck(m_mutex);
        m_index.foreach(fn);
    }

    PointIndex guarded_clone()
    {
        std::lock_guard<Mutex> lck(m_mutex);
        return m_index;
    }
};

// Helper function for pillar interconnection where pairs of already connected
// pillars should be checked for not to be processed again. This can be done
// in constant time with a set of hash values uniquely representing a pair of
// integers. The order of numbers within the pair should not matter, it has
// the same unique hash. The hash value has to have twice as many bits as the
// arguments need. If the same integral type is used for args and return val,
// make sure the arguments use only the half of the type's bit depth.
template<class I, class DoubleI = IntegerOnly<I>>
IntegerOnly<DoubleI> pairhash(I a, I b)
{
    using std::ceil; using std::log2; using std::max; using std::min;
    static const auto constexpr Ibits = int(sizeof(I) * CHAR_BIT);
    static const auto constexpr DoubleIbits = int(sizeof(DoubleI) * CHAR_BIT);
    static const auto constexpr shift = DoubleIbits / 2 < Ibits ? Ibits / 2 : Ibits;

    I g = min(a, b), l = max(a, b);

    // Assume the hash will fit into the output variable
    assert((g ? (ceil(log2(g))) : 0) <= shift);
    assert((l ? (ceil(log2(l))) : 0) <= shift);

    return (DoubleI(g) << shift) + l;
}

template<class C, class Hit = IndexedMesh::hit_result>
Hit min_hit(const C &hits)
{
    auto mit = std::min_element(hits.begin(), hits.end(),
                                [](const Hit &h1, const Hit &h2) {
                                    return h1.distance() < h2.distance();
                                });

    return *mit;
}

template<class Ex>
IndexedMesh::hit_result bridge_mesh_intersect(Ex &&              ex,
                                              const IndexedMesh &mesh,
                                              const Vec3d &      src,
                                              const Vec3d &      dir,
                                              double             r,
                                              double             sd)
{
    static const size_t SAMPLES = 8;
    PointRing<SAMPLES>  ring{dir};

    using Hit = IndexedMesh::hit_result;

    // Hit results
    std::array<Hit, SAMPLES> hits;

    execution::for_each(
        ex, size_t(0), hits.size(),
        [&mesh, r, src, /*ins_check,*/ &ring, dir, sd, &hits](size_t i) {
            Hit &hit = hits[i];

            // Point on the circle on the pin sphere
            Vec3d p = ring.get(i, src, r + sd);

            auto hr = mesh.query_ray_hit(p + r * dir, dir);

            if (/*ins_check && */ hr.is_inside()) {
                if (hr.distance() > 2 * r + sd)
                    hit = Hit(0.0);
                else {
                    // re-cast the ray from the outside of the object
                    hit = mesh.query_ray_hit(p + (hr.distance() + EPSILON) * dir,
                                             dir);
                }
            } else
                hit = hr;
        });

    return min_hit(hits);
}

template<class Ex>
IndexedMesh::hit_result bridge_mesh_intersect(Ex &&              ex,
                                              const IndexedMesh &mesh,
                                              const Vec3d &      src,
                                              const Vec3d &      dst,
                                              double             r_src,
                                              double             r_dst,
                                              double             sd)
{
    constexpr size_t SAMPLES = 8;
    Vec3d D = (dst - src);
    Vec3d dir = D.normalized();
    PointRing<SAMPLES>  ring{dir};

    using Hit = IndexedMesh::hit_result;

    // Hit results
    std::array<Hit, SAMPLES> hits;

    execution::for_each(
        ex, size_t(0), hits.size(),
        [&mesh, r_src, r_dst, src, dst, /*ins_check,*/ &ring, dir, sd, &hits](size_t i) {
            Hit &hit = hits[i];

            // Point on the circle on the pin sphere
            Vec3d p_src = ring.get(i, src, r_src + sd);
            Vec3d p_dst = ring.get(i, dst, r_dst + sd);
            Vec3d raydir = (p_dst - p_src).normalized();

            auto hr = mesh.query_ray_hit(p_src + r_src * raydir, raydir);

            if (/*ins_check && */ hr.is_inside()) {
                if (hr.distance() > 2 * r_src + sd)
                    hit = Hit(0.0);
                else {
                    // re-cast the ray from the outside of the object
                    hit = mesh.query_ray_hit(p_src + (hr.distance() + EPSILON) * raydir,
                                             raydir);
                }
            } else
                hit = hr;
        });

    return min_hit(hits);
}

class SupportTreeBuildsteps {
    const SupportTreeConfig& m_cfg;
    const IndexedMesh& m_mesh;
    const std::vector<SupportPoint>& m_support_pts;

    using PtIndices = std::vector<unsigned>;

    PtIndices m_iheads;            // support points with pinhead
    PtIndices m_iheads_onmodel;
    PtIndices m_iheadless;         // headless support points
    
    std::map<unsigned, IndexedMesh::hit_result> m_head_to_ground_scans;

    // normals for support points from model faces.
    PointSet  m_support_nmls;

    // Clusters of points which can reach the ground directly and can be
    // bridged to one central pillar
    std::vector<PtIndices> m_pillar_clusters;

    // This algorithm uses the SupportTreeBuilder class to fill gradually
    // the support elements (heads, pillars, bridges, ...)
    SupportTreeBuilder& m_builder;

    // support points in Eigen/IGL format
    PointSet m_points;

    // throw if canceled: It will be called many times so a shorthand will
    // come in handy.
    ThrowOnCancel m_thr;

    // A spatial index to easily find strong pillars to connect to.
    PillarIndex m_pillar_index;

    // When bridging heads to pillars... TODO: find a cleaner solution
    ccr::BlockingMutex m_bridge_mutex;

    inline IndexedMesh::hit_result ray_mesh_intersect(const Vec3d& s, 
                                                      const Vec3d& dir)
    {
        return m_mesh.query_ray_hit(s, dir);
    }

    // This function will test if a future pinhead would not collide with the
    // model geometry. It does not take a 'Head' object because those are
    // created after this test. Parameters: s: The touching point on the model
    // surface. dir: This is the direction of the head from the pin to the back
    // r_pin, r_back: the radiuses of the pin and the back sphere width: This
    // is the full width from the pin center to the back center m: The object
    // mesh.
    // The return value is the hit result from the ray casting. If the starting
    // point was inside the model, an "invalid" hit_result will be returned
    // with a zero distance value instead of a NAN. This way the result can
    // be used safely for comparison with other distances.
    IndexedMesh::hit_result pinhead_mesh_intersect(
        const Vec3d& s,
        const Vec3d& dir,
        double r_pin,
        double r_back,
        double width,
        double safety_d);

    IndexedMesh::hit_result pinhead_mesh_intersect(
        const Vec3d& s,
        const Vec3d& dir,
        double r_pin,
        double r_back,
        double width)
    {
        return pinhead_mesh_intersect(s, dir, r_pin, r_back, width,
                                      r_back * m_cfg.safety_distance_mm /
                                          m_cfg.head_back_radius_mm);
    }

    // Checking bridge (pillar and stick as well) intersection with the model.
    // If the function is used for headless sticks, the ins_check parameter
    // have to be true as the beginning of the stick might be inside the model
    // geometry.
    // The return value is the hit result from the ray casting. If the starting
    // point was inside the model, an "invalid" hit_result will be returned
    // with a zero distance value instead of a NAN. This way the result can
    // be used safely for comparison with other distances.
    IndexedMesh::hit_result bridge_mesh_intersect(
        const Vec3d& s,
        const Vec3d& dir,
        double r,
        double safety_d);

    IndexedMesh::hit_result bridge_mesh_intersect(
        const Vec3d& s,
        const Vec3d& dir,
        double r)
    {
        return bridge_mesh_intersect(s, dir, r,
                                     r * m_cfg.safety_distance_mm /
                                         m_cfg.head_back_radius_mm);
    }
    
    template<class...Args>
    inline double bridge_mesh_distance(Args&&...args) {
        return bridge_mesh_intersect(std::forward<Args>(args)...).distance();
    }

    // Helper function for interconnecting two pillars with zig-zag bridges.
    bool interconnect(const Pillar& pillar, const Pillar& nextpillar);

    // For connecting a head to a nearby pillar.
    bool connect_to_nearpillar(const Head& head, long nearpillar_id);
    
    // Find route for a head to the ground. Inserts additional bridge from the
    // head to the pillar if cannot create pillar directly.
    // The optional dir parameter is the direction of the bridge which is the
    // direction of the pinhead if omitted.
    bool connect_to_ground(Head& head, const Vec3d &dir);
    inline bool connect_to_ground(Head& head);
    
    bool connect_to_model_body(Head &head);

    bool search_pillar_and_connect(const Head& source);
    
    // This is a proxy function for pillar creation which will mind the gap
    // between the pad and the model bottom in zero elevation mode.
    // jp is the starting junction point which needs to be routed down.
    // sourcedir is the allowed direction of an optional bridge between the
    // jp junction and the final pillar.
    bool create_ground_pillar(const Vec3d &jp,
                              const Vec3d &sourcedir,
                              double       radius,
                              long         head_id = SupportTreeNode::ID_UNSET);

    void add_pillar_base(long pid)
    {
        m_builder.add_pillar_base(pid, m_cfg.base_height_mm, m_cfg.base_radius_mm);
    }

    std::optional<DiffBridge> search_widening_path(const Vec3d &jp,
                                                   const Vec3d &dir,
                                                   double       radius,
                                                   double       new_radius);

public:
    SupportTreeBuildsteps(SupportTreeBuilder & builder, const SupportableMesh &sm);

    // Now let's define the individual steps of the support generation algorithm

    // Filtering step: here we will discard inappropriate support points
    // and decide the future of the appropriate ones. We will check if a
    // pinhead is applicable and adjust its angle at each support point. We
    // will also merge the support points that are just too close and can
    // be considered as one.
    void filter();

    // Pinhead creation: based on the filtering results, the Head objects
    // will be constructed (together with their triangle meshes).
    void add_pinheads();

    // Further classification of the support points with pinheads. If the
    // ground is directly reachable through a vertical line parallel to the
    // Z axis we consider a support point as pillar candidate. If touches
    // the model geometry, it will be marked as non-ground facing and
    // further steps will process it. Also, the pillars will be grouped
    // into clusters that can be interconnected with bridges. Elements of
    // these groups may or may not be interconnected. Here we only run the
    // clustering algorithm.
    void classify();

    // Step: Routing the ground connected pinheads, and interconnecting
    // them with additional (angled) bridges. Not all of these pinheads
    // will be a full pillar (ground connected). Some will connect to a
    // nearby pillar using a bridge. The max number of such side-heads for
    // a central pillar is limited to avoid bad weight distribution.
    void routing_to_ground();

    // Step: routing the pinheads that would connect to the model surface
    // along the Z axis downwards. For now these will actually be connected with
    // the model surface with a flipped pinhead. In the future here we could use
    // some smart algorithms to search for a safe path to the ground or to a
    // nearby pillar that can hold the supported weight.
    void routing_to_model();

    void interconnect_pillars();

    inline void merge_result() { m_builder.merged_mesh(); }

    static bool execute(SupportTreeBuilder & builder, const SupportableMesh &sm);
};

}
}

#endif // SLASUPPORTTREEALGORITHM_H
