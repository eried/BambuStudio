#ifndef slic3r_PerimeterGenerator_hpp_
#define slic3r_PerimeterGenerator_hpp_

#include "libslic3r.h"
#include <vector>
#include "Flow.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"
#include "SurfaceCollection.hpp"

namespace Slic3r {
class LayerRegion;
class PrintRegion;

struct PerimeterRegion
{
    const PrintRegion *region;
    ExPolygons         expolygons;
    BoundingBox        bbox;

    explicit PerimeterRegion(const LayerRegion &layer_region);

    // If there is any incompatibility, we don't need to create separate LayerRegions.
    // Because it is enough to split perimeters by PerimeterRegions.
    static bool has_compatible_perimeter_regions(const PrintRegionConfig &config, const PrintRegionConfig &other_config);

    static void merge_compatible_perimeter_regions(std::vector<PerimeterRegion> &perimeter_regions);
};

using PerimeterRegions = std::vector<PerimeterRegion>;

class PerimeterGenerator {
public:
    // Inputs:
    const SurfaceCollection     *slices;
    const ExPolygons            *upper_slices;
    const ExPolygons            *lower_slices;
    double                       layer_height;
    int                          layer_id;
    Flow                         perimeter_flow;
    Flow                         ext_perimeter_flow;
    Flow                         overhang_flow;
    Flow                         solid_infill_flow;
    const PrintRegionConfig     *config;
    const PrintObjectConfig     *object_config;
    const PrintConfig           *print_config;
    const PerimeterRegions      *perimeter_regions;
    // Outputs:
    ExtrusionEntityCollection   *loops;
    ExtrusionEntityCollection   *gap_fill;
    SurfaceCollection           *fill_surfaces;
    //BBS
    ExPolygons                  *fill_no_overlap;

    //BBS
    Flow                        smaller_ext_perimeter_flow;
    std::vector<Polygons>       m_lower_polygons_series;
    std::vector<Polygons>       m_external_lower_polygons_series;
    std::vector<Polygons>       m_smaller_external_lower_polygons_series;
    std::pair<double, double>   m_lower_overhang_dist_boundary;
    std::pair<double, double>   m_external_overhang_dist_boundary;
    std::pair<double, double>   m_smaller_external_overhang_dist_boundary;
    std::vector<LoopNode>       *loop_nodes;

    PerimeterGenerator(
        // Input:
        const SurfaceCollection*    slices,
        double                      layer_height,
        Flow                        flow,
        const PrintRegionConfig*    config,
        const PrintObjectConfig*    object_config,
        const PrintConfig*          print_config,
        const bool                  spiral_mode,
        // Output:
        // Loops with the external thin walls
        ExtrusionEntityCollection*  loops,
        // Gaps without the thin walls
        ExtrusionEntityCollection*  gap_fill,
        // Infills without the gap fills
        SurfaceCollection*          fill_surfaces,
        //BBS
        ExPolygons*                 fill_no_overlap,
        std::vector<LoopNode>       *loop_nodes)
        : slices(slices), upper_slices(nullptr), lower_slices(nullptr), layer_height(layer_height),
            layer_id(-1), perimeter_flow(flow), ext_perimeter_flow(flow),
            overhang_flow(flow), solid_infill_flow(flow),
            config(config), object_config(object_config), print_config(print_config),
            m_spiral_vase(spiral_mode),
            m_scaled_resolution(scaled<double>(print_config->resolution.value > EPSILON ? print_config->resolution.value : EPSILON)), loops(loops),
        gap_fill(gap_fill),
        fill_surfaces(fill_surfaces),
        fill_no_overlap(fill_no_overlap),
        loop_nodes(loop_nodes),
            m_ext_mm3_per_mm(-1), m_mm3_per_mm(-1), m_mm3_per_mm_overhang(-1), m_ext_mm3_per_mm_smaller_width(-1)
        {}

    void        process_classic();
    void        process_arachne();

    // to save memory, directly modify top
    bool        should_enable_top_one_wall(const ExPolygons& original_expolys, ExPolygons& top);

    void        add_infill_contour_for_arachne( ExPolygons infill_contour, int loops, coord_t ext_perimeter_spacing, coord_t perimeter_spacing, coord_t min_perimeter_infill_spacing, coord_t spacing, bool is_inner_part );

    double      ext_mm3_per_mm()        const { return m_ext_mm3_per_mm; }
    double      mm3_per_mm()            const { return m_mm3_per_mm; }
    double      mm3_per_mm_overhang()   const { return m_mm3_per_mm_overhang; }
    //BBS
    double      smaller_width_ext_mm3_per_mm()   const { return m_ext_mm3_per_mm_smaller_width; }
    Polygons    lower_slices_polygons() const { return m_lower_slices_polygons; }

private:
    std::vector<Polygons>     generate_lower_polygons_series(float width);
    std::pair<double, double> dist_boundary(double width);

private:
    bool        m_spiral_vase;
    double      m_scaled_resolution;
    double      m_ext_mm3_per_mm;
    double      m_mm3_per_mm;
    double      m_mm3_per_mm_overhang;
    //BBS
    double      m_ext_mm3_per_mm_smaller_width;
    Polygons    m_lower_slices_polygons;
};

}

#endif
