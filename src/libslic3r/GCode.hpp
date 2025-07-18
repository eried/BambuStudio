#ifndef slic3r_GCode_hpp_
#define slic3r_GCode_hpp_

#include "libslic3r.h"
#include "ExPolygon.hpp"
#include "GCodeWriter.hpp"
#include "Layer.hpp"
#include "Point.hpp"
#include "PlaceholderParser.hpp"
#include "PrintConfig.hpp"
#include "GCode/AvoidCrossingPerimeters.hpp"
#include "GCode/GCodeEditor.hpp"
#include "GCode/RetractWhenCrossingPerimeters.hpp"
#include "GCode/SpiralVase.hpp"
#include "GCode/ToolOrdering.hpp"
#include "GCode/WipeTower.hpp"
#include "GCode/SeamPlacer.hpp"
#include "GCode/GCodeProcessor.hpp"
#include "EdgeGrid.hpp"
#include "GCode/ThumbnailData.hpp"
#include "libslic3r/ObjectID.hpp"
#include "GCode/TimelapsePosPicker.hpp"

#include <cfloat>
#include <memory>
#include <map>
#include <set>
#include <string>

#ifdef HAS_PRESSURE_EQUALIZER
#include "GCode/PressureEqualizer.hpp"
#endif /* HAS_PRESSURE_EQUALIZER */

namespace Slic3r {

// Forward declarations.
class GCode;

namespace { struct Item; }
struct PrintInstance;
class ConstPrintObjectPtrsAdaptor;

class OozePrevention {
public:
    bool enable;
    Points standby_points;

    OozePrevention() : enable(false) {}
    std::string pre_toolchange(GCode &gcodegen);
    std::string post_toolchange(GCode &gcodegen);

private:
    int _get_temp(GCode &gcodegen);
};

class Wipe {
public:
    bool enable;
    Polyline path;

    Wipe() : enable(false) {}
    bool has_path() const { return !this->path.points.empty(); }
    void reset_path() { this->path = Polyline(); }
    std::string wipe(GCode &gcodegen, bool toolchange = false, bool is_last = false);
};

class WipeTowerIntegration {
public:
    WipeTowerIntegration(
        const PrintConfig                                           &print_config,
        // BBS: add partplate logic
        const int                                                    plate_idx,
        const Vec3d                                                  plate_origin,
        const std::vector<WipeTower::ToolChangeResult>              &priming,
        const std::vector<std::vector<WipeTower::ToolChangeResult>> &tool_changes,
        const WipeTower::ToolChangeResult                           &final_purge) :
        m_left(/*float(print_config.wipe_tower_x.value)*/ 0.f),
        m_right(float(/*print_config.wipe_tower_x.value +*/ print_config.prime_tower_width.value)),
        m_wipe_tower_pos(float(print_config.wipe_tower_x.get_at(plate_idx)), float(print_config.wipe_tower_y.get_at(plate_idx))),
        m_wipe_tower_rotation(float(print_config.wipe_tower_rotation_angle)),
        m_priming(priming),
        m_tool_changes(tool_changes),
        m_final_purge(final_purge),
        m_layer_idx(-1),
        m_tool_change_idx(0),
        m_plate_origin(plate_origin),
        m_single_extruder_multi_material(print_config.single_extruder_multi_material),
        m_enable_timelapse_print(print_config.timelapse_type.value == TimelapseType::tlSmooth),
        m_is_first_print(true),
        m_print_config(&print_config)
    {
        // initialize with the extruder offset of master extruder id
        m_extruder_offsets.resize(print_config.filament_map.size(), print_config.extruder_offset.get_at(print_config.master_extruder_id.value - 1));
        const auto& filament_map = print_config.filament_map.values; // 1 based idx
        for (size_t idx = 0; idx < filament_map.size(); ++idx)
            m_extruder_offsets[idx] = print_config.extruder_offset.get_at(filament_map[idx] - 1);
    }

    std::string prime(GCode &gcodegen);
    void next_layer() { ++ m_layer_idx; m_tool_change_idx = 0; }
    std::string tool_change(GCode &gcodegen, int extruder_id, bool finish_layer);
    bool is_empty_wipe_tower_gcode(GCode &gcodegen, int extruder_id, bool finish_layer);
    std::string finalize(GCode &gcodegen);
    std::vector<float> used_filament_length() const;

    bool is_first_print() const { return m_is_first_print;}
    void set_is_first_print(bool is) { m_is_first_print = is; }

    bool enable_timelapse_print() const { return m_enable_timelapse_print; }
    void set_wipe_tower_depth(float depth) { m_wipe_tower_depth = depth; }
    void set_wipe_tower_bbx(const BoundingBoxf & bbx) { m_wipe_tower_bbx = bbx; }
    void set_rib_offset(const Vec2f &rib_offset) { m_rib_offset = rib_offset; }

private:
    WipeTowerIntegration& operator=(const WipeTowerIntegration&);
    std::string append_tcr(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr, int new_extruder_id, double z = -1.) const;
    Polyline generate_path_to_wipe_tower(const Point &start_pos, const Point &end_pos, const BoundingBox &avoid_polygon, const BoundingBox &printer_bbx) const;

    // Postprocesses gcode: rotates and moves G1 extrusions and returns result
    std::string post_process_wipe_tower_moves(const WipeTower::ToolChangeResult& tcr, const Vec2f& translation, float angle) const;

    // Left / right edges of the wipe tower, for the planning of wipe moves.
    const float                                                  m_left;
    const float                                                  m_right;
    const Vec2f                                                  m_wipe_tower_pos;
    const float                                                  m_wipe_tower_rotation;
    std::vector<Vec2d>                                           m_extruder_offsets;

    // Reference to cached values at the Printer class.
    const std::vector<WipeTower::ToolChangeResult>              &m_priming;
    const std::vector<std::vector<WipeTower::ToolChangeResult>> &m_tool_changes;
    const WipeTower::ToolChangeResult                           &m_final_purge;
    // Current layer index.
    int                                                          m_layer_idx;
    int                                                          m_tool_change_idx;
    double                                                       m_last_wipe_tower_print_z = 0.f;

    // BBS
    Vec3d                                                        m_plate_origin;
    bool                                                         m_single_extruder_multi_material;
    bool                                                         m_enable_timelapse_print;
    bool                                                         m_is_first_print;
    const PrintConfig *                                          m_print_config;
    float                                                        m_wipe_tower_depth;
    BoundingBoxf                                                 m_wipe_tower_bbx;
    Vec2f                                                        m_rib_offset{Vec2f(0, 0)};
};

class ColorPrintColors
{
    static const std::vector<std::string> Colors;
public:
    static const std::vector<std::string>& get() { return Colors; }
};

class GCode {
public:
    GCode() :
    	m_origin(Vec2d::Zero()),
        m_enable_loop_clipping(true),
        m_enable_cooling_markers(false),
        m_enable_extrusion_role_markers(false),
        m_last_processor_extrusion_role(erNone),
        m_layer_count(0),
        m_layer_index(-1),
        m_layer(nullptr),
        m_object_layer_over_raft(false),
        //m_volumetric_speed(0),
        m_last_scarf_seam_flag(false),
        m_last_pos_defined(false),
        m_last_extrusion_role(erNone),
        m_last_width(0.0f),
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_last_mm3_per_mm(0.0),
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_brim_done(false),
        m_second_layer_things_done(false),
        m_silent_time_estimator_enabled(false),
        m_last_obj_copy(nullptr, Point(std::numeric_limits<coord_t>::max(), std::numeric_limits<coord_t>::max())),
        // BBS
        m_toolchange_count(0),
        m_nominal_z(0.)
        {}
    ~GCode() = default;

    // throws std::runtime_exception on error,
    // throws CanceledException through print->throw_if_canceled().
    void            do_export(Print* print, const char* path, GCodeProcessorResult* result = nullptr, ThumbnailsGeneratorCallback thumbnail_cb = nullptr);
    void            export_layer_filaments(GCodeProcessorResult* result);
    //BBS: set offset for gcode writer
    void set_gcode_offset(double x, double y) { m_writer.set_xy_offset(x, y); m_processor.set_xy_offset(x, y);}

    // Exported for the helper classes (OozePrevention, Wipe) and for the Perl binding for unit tests.
    const Vec2d&    origin() const { return m_origin; }
    void            set_origin(const Vec2d &pointf);
    void            set_origin(const coordf_t x, const coordf_t y) { this->set_origin(Vec2d(x, y)); }
    const Point&    last_pos() const { return m_last_pos; }
    const bool&     last_scarf_seam_flag() const { return m_last_scarf_seam_flag; }
    Vec2d           point_to_gcode(const Point &point) const;
    Point           gcode_to_point(const Vec2d &point) const;
    const FullPrintConfig &config() const { return m_config; }
    const Layer*    layer() const { return m_layer; }
    GCodeWriter&    writer() { return m_writer; }
    const GCodeWriter& writer() const { return m_writer; }
    PlaceholderParser& placeholder_parser() { return m_placeholder_parser; }
    const PlaceholderParser& placeholder_parser() const { return m_placeholder_parser; }
    // Process a template through the placeholder parser, collect error messages to be reported
    // inside the generated string and after the G-code export finishes.
    std::string     placeholder_parser_process(const std::string &name, const std::string &templ, unsigned int current_extruder_id, const DynamicConfig *config_override = nullptr);
    bool            enable_cooling_markers() const { return m_enable_cooling_markers; }

    // For Perl bindings, to be used exclusively by unit tests.
    unsigned int    layer_count() const { return m_layer_count; }
    void            set_layer_count(unsigned int value) { m_layer_count = value; }
    void            apply_print_config(const PrintConfig &print_config);

    // OrcaSlicer
    std::string set_object_info(Print* print);

    // append full config to the given string
    static void append_full_config(const DynamicPrintConfig &cfg, std::string &str);

    // BBS: detect lift type in needs_retraction
    bool        needs_retraction(const Polyline &travel, ExtrusionRole role, LiftType &lift_type);
    std::string retract(bool toolchange = false, bool is_last_retraction = false, LiftType lift_type = LiftType::SpiralLift, bool apply_instantly = false);
    std::string unretract() { return m_writer.unlift() + m_writer.unretract(); }
    //BBS
    bool is_BBL_Printer();

    BoundingBoxf first_layer_projection(const Print& print) const;

    // Object and support extrusions of the same PrintObject at the same print_z.
    // public, so that it could be accessed by free helper functions from GCode.cpp
    struct LayerToPrint
    {
        LayerToPrint() : object_layer(nullptr), support_layer(nullptr), original_object(nullptr) {}
        const Layer* 		object_layer;
        const SupportLayer* support_layer;
        const PrintObject*  original_object; //BBS: used for shared object logic
        const Layer* 		layer()   const
        {
            if (object_layer != nullptr)
                return object_layer;

            if (support_layer != nullptr)
                return support_layer;

            return nullptr;
        }

        const PrintObject* 	object()   const
        {
            return (this->layer() != nullptr) ? this->layer()->object() : nullptr;
        }
        coordf_t            print_z() const
        {
            coordf_t sum_z = 0.;
            size_t count = 0;
            if (object_layer != nullptr) {
                sum_z += object_layer->print_z;
                count++;
            }

            if (support_layer != nullptr) {
                sum_z += support_layer->print_z;
                count++;
            }

            return sum_z / count;
        }
    };

private:
    class GCodeOutputStream {
    public:
        GCodeOutputStream(FILE *f, GCodeProcessor &processor) : f(f), m_processor(processor) {}
        ~GCodeOutputStream() { this->close(); }

        bool is_open() const { return f; }
        bool is_error() const;

        void flush();
        void close();

        // Write a string into a file.
        void write(const std::string& what) { this->write(what.c_str()); }
        void write(const char* what);

        // Write a string into a file.
        // Add a newline, if the string does not end with a newline already.
        // Used to export a custom G-code section processed by the PlaceholderParser.
        void writeln(const std::string& what);

        // Formats and write into a file the given data.
        void write_format(const char* format, ...);

    private:
        FILE *f = nullptr;
        GCodeProcessor &m_processor;
    };
    void            _do_export(Print &print, GCodeOutputStream &file, ThumbnailsGeneratorCallback thumbnail_cb);

    static std::vector<LayerToPrint>        		                   collect_layers_to_print(const PrintObject &object);
    static std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> collect_layers_to_print(const Print &print);

    struct LayerResult {
        std::string gcode;
        size_t      layer_id;
        // Is spiral vase post processing enabled for this layer?
        bool        spiral_vase_enable { false };
        // Should the cooling buffer content be flushed at the end of this layer?
        bool        cooling_buffer_flush { false };
        // the layer store pos of gcode
        size_t      gcode_store_pos = 0;
        //store each layer_time
        float       layer_time = 0;
        LayerResult() = default;
        LayerResult(const std::string& gcode_, const size_t layer_id_, const bool spiral_vase_enable_, const bool cooling_buffer_flush_, const size_t gcode_store_pos_ = static_cast<size_t>(-1)) :
            gcode(gcode_), layer_id(layer_id_), spiral_vase_enable(spiral_vase_enable_), cooling_buffer_flush(cooling_buffer_flush_), gcode_store_pos(gcode_store_pos_){}
        LayerResult(const LayerResult& other) = default;
        LayerResult& operator=(const LayerResult& other) = default;
        LayerResult(LayerResult&& other) noexcept {
            gcode = std::move(other.gcode);
            layer_id = other.layer_id;
            spiral_vase_enable = other.spiral_vase_enable;
            cooling_buffer_flush = other.cooling_buffer_flush;
            gcode_store_pos = other.gcode_store_pos;
            layer_time = other.layer_time;
        }

        LayerResult& operator=(LayerResult&& other) noexcept {
            if (this != &other) {
                gcode = std::move(other.gcode);
                layer_id = other.layer_id;
                spiral_vase_enable = other.spiral_vase_enable;
                cooling_buffer_flush = other.cooling_buffer_flush;
                gcode_store_pos = other.gcode_store_pos;
                layer_time = other.layer_time;
            }
            return *this;
        }
    };
    LayerResult process_layer(
        const Print                     &print,
        // Set of object & print layers of the same PrintObject and with the same print_z.
        const std::vector<LayerToPrint> &layers,
        const LayerTools  				&layer_tools,
        const bool                       last_layer,
		// Pairs of PrintObject index and its instance index.
		const std::vector<const PrintInstance*> *ordering,
        // idientiy timelapse pos
        const int                        most_used_extruder,
        // If set to size_t(-1), then print all copies of all objects.
        // Otherwise print a single copy of a single object.
        const size_t                     single_object_idx = size_t(-1),
        // BBS
        const bool                       prime_extruder = false);
    // Process all layers of all objects (non-sequential mode) with a parallel pipeline:
    // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
    // and export G-code into file.
    void process_layers(
        const Print                                                         &print,
        const ToolOrdering                                                  &tool_ordering,
        const std::vector<const PrintInstance*>                             &print_object_instances_ordering,
        const std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>>   &layers_to_print,
        GCodeOutputStream                                                   &output_stream);
    // Process all layers of a single object instance (sequential mode) with a parallel pipeline:
    // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
    // and export G-code into file.
    void process_layers(
        const Print                             &print,
        const ToolOrdering                      &tool_ordering,
        std::vector<LayerToPrint>                layers_to_print,
        const size_t                             single_object_idx,
        GCodeOutputStream                       &output_stream,
        // BBS
        const bool                               prime_extruder = false);

    //BBS
    void check_placeholder_parser_failed();
    size_t cur_extruder_index() const;
    size_t get_extruder_id(unsigned int filament_id) const;
    void set_extrude_acceleration(bool is_first_layer);

    void            set_last_pos(const Point &pos) { m_last_pos = pos; m_last_pos_defined = true; }
    void            set_last_scarf_seam_flag(bool flag) { m_last_scarf_seam_flag = flag; }
    bool            last_pos_defined() const { return m_last_pos_defined; }
    void            set_extruders(const std::vector<unsigned int> &extruder_ids);
    std::string     preamble();
    // BBS
    std::string     change_layer(coordf_t print_z);
    std::string     extrude_entity(const ExtrusionEntity &entity, std::string description = "", double speed = -1.);
    std::string     extrude_loop(ExtrusionLoop loop, std::string description, double speed = -1.);
    std::string     extrude_multi_path(ExtrusionMultiPath multipath, std::string description = "", double speed = -1.);
    std::string     extrude_path(ExtrusionPath path, std::string description = "", double speed = -1.);

    //smooth speed function
    void            smooth_speed_discontinuity_area(ExtrusionPaths &paths);
    std::vector<ExtrusionPaths>  merge_same_speed_paths(const ExtrusionPaths &paths);

    // slow down by height
    bool slowDownByHeight(double& maxSpeed, double& maxAcc, const ExtrusionPath& path);

    // Extruding multiple objects with soluble / non-soluble / combined supports
    // on a multi-material printer, trying to minimize tool switches.
    // Following structures sort extrusions by the extruder ID, by an order of objects and object islands.
    struct ObjectByExtruder
    {
        ObjectByExtruder() : support(nullptr), support_extrusion_role(erNone) {}
        const ExtrusionEntityCollection  *support;
        // erSupportMaterial / erSupportMaterialInterface / erSupportTransition or erMixed.
        ExtrusionRole                     support_extrusion_role;

        struct Island
        {
            struct Region {
            	// Non-owned references to LayerRegion::perimeters::entities
            	// std::vector<const ExtrusionEntity*> would be better here, but there is no way in C++ to convert from std::vector<T*> std::vector<const T*> without copying.
                ExtrusionEntitiesPtr perimeters;
            	// Non-owned references to LayerRegion::fills::entities
                ExtrusionEntitiesPtr infills;
                std::vector<const WipingExtrusions::ExtruderPerCopy*> infills_overrides;
                std::vector<const WipingExtrusions::ExtruderPerCopy*> perimeters_overrides;

	            enum Type {
	            	PERIMETERS,
	            	INFILL,
	            };

                // Appends perimeter/infill entities and writes don't indices of those that are not to be extruder as part of perimeter/infill wiping
                void append(const Type type, const ExtrusionEntityCollection* eec, const WipingExtrusions::ExtruderPerCopy* copy_extruders);
            };


            std::vector<Region> by_region;                                    // all extrusions for this island, grouped by regions

            // Fills in by_region_per_copy_cache and returns its reference.
            const std::vector<Region>& by_region_per_copy(std::vector<Region> &by_region_per_copy_cache, unsigned int copy, unsigned int extruder, bool wiping_entities = false) const;
        };
        std::vector<Island>         islands;
    };

	struct InstanceToPrint
	{
		InstanceToPrint(ObjectByExtruder &object_by_extruder, size_t layer_id, const PrintObject &print_object, size_t instance_id, size_t label_object_id) :
			object_by_extruder(object_by_extruder), layer_id(layer_id), print_object(print_object), instance_id(instance_id), label_object_id(label_object_id) {}

		// Repository
		ObjectByExtruder		&object_by_extruder;
		// Index into std::vector<LayerToPrint>, which contains Object and Support layers for the current print_z, collected for a single object, or for possibly multiple objects with multiple instances.
		const size_t       		 layer_id;
		const PrintObject 		&print_object;
		// Instance idx of the copy of a print object.
		const size_t			 instance_id;
        //BBS: Unique id to label object to support skiping during printing
        const size_t             label_object_id;
	};

	std::vector<InstanceToPrint> sort_print_object_instances(
		std::vector<ObjectByExtruder> 					&objects_by_extruder,
		// Object and Support layers for the current print_z, collected for a single object, or for possibly multiple objects with multiple instances.
		const std::vector<LayerToPrint> 				&layers,
		// Ordering must be defined for normal (non-sequential print).
		const std::vector<const PrintInstance*>     	*ordering,
		// For sequential print, the instance of the object to be printing has to be defined.
		const size_t                     				 single_object_instance_idx);

    std::string     extrude_perimeters(const Print &print, const std::vector<ObjectByExtruder::Island::Region> &by_region);
    std::string     extrude_infill(const Print &print, const std::vector<ObjectByExtruder::Island::Region> &by_region, bool ironing);
    std::string     extrude_support(const ExtrusionEntityCollection &support_fills);

    std::string travel_to(const Point &point, ExtrusionRole role, std::string comment, double z = DBL_MAX);

    void reset_last_acceleration();
    // BBS
    LiftType to_lift_type(ZHopType z_hop_types);

    std::string     set_extruder(unsigned int extruder_id, double print_z, bool by_object=false);
    std::set<ObjectID>              m_objsWithBrim; // indicates the objs with brim
    std::set<ObjectID>              m_objSupportsWithBrim; // indicates the objs' supports with brim
    // Cache for custom seam enforcers/blockers for each layer.
    SeamPlacer                          m_seam_placer;

    /* Origin of print coordinates expressed in unscaled G-code coordinates.
       This affects the input arguments supplied to the extrude*() and travel_to()
       methods. */
    Vec2d                               m_origin;
    FullPrintConfig                     m_config;
    DynamicConfig                       m_calib_config;
    // scaled G-code resolution
    double                              m_scaled_resolution;
    GCodeWriter                         m_writer;
    PlaceholderParser                   m_placeholder_parser;
    // For random number generator etc.
    PlaceholderParser::ContextData      m_placeholder_parser_context;
    // Collection of templates, on which the placeholder substitution failed.
    std::map<std::string, std::string>  m_placeholder_parser_failed_templates;
    OozePrevention                      m_ooze_prevention;
    Wipe                                m_wipe;
    AvoidCrossingPerimeters             m_avoid_crossing_perimeters;
    RetractWhenCrossingPerimeters       m_retract_when_crossing_perimeters;
    TimelapsePosPicker                  m_timelapse_pos_picker;
    bool                                m_enable_loop_clipping;
    // If enabled, the G-code generator will put following comments at the ends
    // of the G-code lines: _EXTRUDE_SET_SPEED, _WIPE, _OVERHANG_FAN_START, _OVERHANG_FAN_END
    // Those comments are received and consumed (removed from the G-code) by the CoolingBuffer.pm Perl module.
    bool                                m_enable_cooling_markers;
    // Markers for the Pressure Equalizer to recognize the extrusion type.
    // The Pressure Equalizer removes the markers from the final G-code.
    bool                                m_enable_extrusion_role_markers;
    // Keeps track of the last extrusion role passed to the processor
    ExtrusionRole                       m_last_processor_extrusion_role;
    // How many times will change_layer() be called?
    // change_layer() will update the progress bar.
    unsigned int                        m_layer_count;
    // Progress bar indicator. Increments from -1 up to layer_count.
    int                                 m_layer_index;
    // Current layer processed. In sequential printing mode, only a single copy will be printed.
    // In non-sequential mode, all its copies will be printed.
    const Layer*                        m_layer;
    // m_layer is an object layer and it is being printed over raft surface.
    bool                                m_object_layer_over_raft;
    //double                              m_volumetric_speed;
    // Support for the extrusion role markers. Which marker is active?
    ExtrusionRole                       m_last_extrusion_role;
    // Support for G-Code Processor
    float                               m_last_height{ 0.0f };
    float                               m_last_layer_z{ 0.0f };
    float                               m_max_layer_z{ 0.0f };
    float                               m_last_width{ 0.0f };
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    double                              m_last_mm3_per_mm;
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    Point                               m_last_pos;
    bool                                m_last_pos_defined;
    bool                                m_last_scarf_seam_flag;
    std::unique_ptr<GCodeEditor>        m_gcode_editer;
    std::unique_ptr<SpiralVase>         m_spiral_vase;
#ifdef HAS_PRESSURE_EQUALIZER
    std::unique_ptr<PressureEqualizer>  m_pressure_equalizer;
#endif /* HAS_PRESSURE_EQUALIZER */
    std::unique_ptr<WipeTowerIntegration> m_wipe_tower;

    // Heights (print_z) at which the skirt has already been extruded.
    std::vector<coordf_t>               m_skirt_done;
    // Has the brim been extruded already? Brim is being extruded only for the first object of a multi-object print.
    bool                                m_brim_done;
    // Flag indicating whether the nozzle temperature changes from 1st to 2nd layer were performed.
    bool                                m_second_layer_things_done;
    // Index of a last object copy extruded.
    std::pair<const PrintObject*, Point> m_last_obj_copy;
    //BBS
    bool m_enable_label_object;
    std::vector<size_t> m_label_objects_ids;
    std::string _encode_label_ids_to_base64(std::vector<size_t> ids);

    // 1 << 0: A1 series cannot supprot traditional timelapse when printing by object (cannot turn on timelapse)
    // 1 << 1: A1 series cannot supprot traditional timelapse with spiral vase mode   (cannot turn on timelapse)
    // 1 << 2: Timelapse in smooth mode without wipe tower (turn on with prompt)
    int m_timelapse_warning_code = 0;
    bool m_support_traditional_timelapse = true;

    bool m_silent_time_estimator_enabled;

    Print *m_print{nullptr};

    std::vector<const PrintObject*> m_printed_objects;

    // Processor
    GCodeProcessor m_processor;

    // BBS
    Print* m_curr_print = nullptr;
    unsigned int m_toolchange_count;
    coordf_t m_nominal_z;
    bool m_need_change_layer_lift_z = false;
    int m_start_gcode_filament = -1;

    std::set<unsigned int>                  m_initial_layer_extruders;
    std::vector<std::vector<unsigned int>>  m_sorted_layer_filaments;
    // BBS
    int get_bed_temperature(const int extruder_id, const bool is_first_layer, const BedType bed_type) const;
    int get_highest_bed_temperature(const bool is_first_layer,const Print &print) const;

    std::string _extrude(const ExtrusionPath &path, std::string description = "", double speed = -1, bool set_holes_and_compensation_speed = false, bool is_first_slope = false);
    ExtrusionPaths set_speed_transition(std::vector<ExtrusionPaths> &paths);
    void split_and_mapping_speed(double other_path_v, double final_v, ExtrusionPaths &this_path, double max_smooth_length, ExtrusionPaths &interpolated_paths, bool split_from_left = true);
    double get_path_speed(const ExtrusionPath &path);
    double get_overhang_degree_corr_speed(float speed, double path_degree);
    double mapping_speed(double dist);
    double get_speed_coor_x(double speed);
    void print_machine_envelope(GCodeOutputStream& file, Print& print, int extruder_id);
    void _print_first_layer_bed_temperature(GCodeOutputStream &file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait);
    void _print_first_layer_extruder_temperatures(GCodeOutputStream &file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait);
    // On the first printing layer. This flag triggers first layer speeds.
    //BBS
    bool                                on_first_layer() const { return m_layer != nullptr && m_layer->id() == 0 && abs(m_layer->bottom_z()) < EPSILON; }
    // To control print speed of 1st object layer over raft interface.
    bool                                object_layer_over_raft() const { return m_object_layer_over_raft; }

    friend ObjectByExtruder& object_by_extruder(
        std::map<unsigned int, std::vector<ObjectByExtruder>> &by_extruder,
        unsigned int                                           extruder_id,
        size_t                                                 object_idx,
        size_t                                                 num_objects);
    friend std::vector<ObjectByExtruder::Island>& object_islands_by_extruder(
        std::map<unsigned int, std::vector<ObjectByExtruder>>  &by_extruder,
        unsigned int                                            extruder_id,
        size_t                                                  object_idx,
        size_t                                                  num_objects,
        size_t                                                  num_islands);

    friend class Wipe;
    friend class WipeTowerIntegration;
    friend class Print;
};

std::vector<const PrintInstance*> sort_object_instances_by_model_order(const Print& print, bool init_order = false);

}

#endif
