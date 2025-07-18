#include "libslic3r/libslic3r.h"
#include "Selection.hpp"

#include "3DScene.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "Gizmos/GLGizmoBase.hpp"
#include "Camera.hpp"
#include "Plater.hpp"
#include "OpenGLManager.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#if ENABLE_ENHANCED_PRINT_VOLUME_FIT
#include "libslic3r/BuildVolume.hpp"
#endif // ENABLE_ENHANCED_PRINT_VOLUME_FIT

#include <GL/glew.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>

#include <CGAL/Simple_cartesian.h>
#include <CGAL/Min_sphere_of_spheres_d.h>
#include <CGAL/Min_sphere_of_points_d_traits_3.h>
static const std::array<float, 4> UNIFORM_SCALE_COLOR = { 0.923f, 0.504f, 0.264f, 1.0f };

namespace Slic3r {
namespace GUI {

Selection::VolumeCache::TransformCache::TransformCache()
    : position(Vec3d::Zero())
    , rotation(Vec3d::Zero())
    , scaling_factor(Vec3d::Ones())
    , mirror(Vec3d::Ones())
    , rotation_matrix(Transform3d::Identity())
    , scale_matrix(Transform3d::Identity())
    , mirror_matrix(Transform3d::Identity())
    , full_tran(Transform3d::Identity())
{
}

Selection::VolumeCache::TransformCache::TransformCache(const Geometry::Transformation& transform)
    : position(transform.get_offset())
    , rotation(transform.get_rotation())
    , scaling_factor(transform.get_scaling_factor())
    , mirror(transform.get_mirror())
    , full_tran(transform)
{
    rotation_matrix = Geometry::assemble_transform(Vec3d::Zero(), rotation);
    scale_matrix = Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), scaling_factor);
    mirror_matrix = Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), Vec3d::Ones(), mirror);
}

Selection::VolumeCache::VolumeCache(const Geometry::Transformation& volume_transform, const Geometry::Transformation& instance_transform)
    : m_volume(volume_transform)
    , m_instance(instance_transform)
{
}

bool Selection::Clipboard::is_sla_compliant() const
{
    if (m_mode == Selection::Volume)
        return false;

    for (const ModelObject* o : m_model->objects) {
        if (o->is_multiparts())
            return false;

        for (const ModelVolume* v : o->volumes) {
            if (v->is_modifier())
                return false;
        }
    }

    return true;
}

Selection::Clipboard::Clipboard()
{
    m_model.reset(new Model);
}

void Selection::Clipboard::reset()
{
    m_model->clear_objects();
}

bool Selection::Clipboard::is_empty() const
{
    return m_model->objects.empty();
}

ModelObject* Selection::Clipboard::add_object()
{
    return m_model->add_object();
}

ModelObject* Selection::Clipboard::get_object(unsigned int id)
{
    return (id < (unsigned int)m_model->objects.size()) ? m_model->objects[id] : nullptr;
}

const ModelObjectPtrs& Selection::Clipboard::get_objects() const
{
    return m_model->objects;
}


Selection::Selection()
    : m_volumes(nullptr)
    , m_model(nullptr)
    , m_enabled(false)
    , m_mode(Instance)
    , m_type(Empty)
    , m_valid(false)
    , m_scale_factor(1.0f)
    , m_dragging(false)
{
    this->set_bounding_boxes_dirty();
}


void Selection::set_volumes(GLVolumePtrs* volumes)
{
    m_volumes = volumes;
    update_valid();
}

// Init shall be called from the OpenGL render function, so that the OpenGL context is initialized!
bool Selection::init()
{
    m_arrow.init_from(straight_arrow(10.0f, 5.0f, 5.0f, 10.0f, 1.0f));
    m_curved_arrow.init_from(circular_arrow(16, 10.0f, 5.0f, 10.0f, 5.0f, 1.0f));

#if ENABLE_RENDER_SELECTION_CENTER
    m_vbo_sphere.init_from(its_make_sphere(0.75, 2*PI/24));
#endif // ENABLE_RENDER_SELECTION_CENTER

    return true;
}

void Selection::set_model(Model* model)
{
    m_model = model;
    update_valid();
}

void Selection::set_mode(EMode mode) {
    m_mode = mode;
}

int Selection::query_real_volume_idx_from_other_view(unsigned int object_idx, unsigned int instance_idx, unsigned int model_volume_idx)
{
    for (int i = 0; i < m_volumes->size(); i++) {
        auto v = (*m_volumes)[i];
        if (v->object_idx() == object_idx && instance_idx == v->instance_idx() && model_volume_idx == v->volume_idx()) {
            return i;
        }
    }
    return -1;
}

void Selection::add(unsigned int volume_idx, bool as_single_selection, bool check_for_already_contained)
{
    if (!m_valid || (unsigned int)m_volumes->size() <= volume_idx)
        return;

    const GLVolume* volume = (*m_volumes)[volume_idx];
    //BBS: multiple wipe tower case should be considered
    // wipe tower is already selected
    //if (is_wipe_tower() && volume->is_wipe_tower)
    //    return;
    if (!m_list.empty() && !is_wipe_tower() && volume->is_wipe_tower && !as_single_selection)
        return;

    bool keep_instance_mode = (m_mode == Instance) && !as_single_selection;
    bool already_contained = check_for_already_contained && contains_volume(volume_idx);

    // resets the current list if needed
    bool needs_reset = as_single_selection && !already_contained;
    needs_reset |= volume->is_wipe_tower;
    needs_reset |= is_wipe_tower() && !volume->is_wipe_tower;
    needs_reset |= as_single_selection && !is_any_modifier() && volume->is_modifier;
    needs_reset |= is_any_modifier() && !volume->is_modifier;
    if (!needs_reset && (is_any_modifier() || is_any_volume())) {
        int obj_index = volume->object_idx();
        int inst_index = volume->instance_idx();
        int first = *(m_list.begin());
        if (first < m_volumes->size()) {
            const GLVolume* volume = (*m_volumes)[first];
            if ((volume->object_idx() != obj_index) || (volume->instance_idx() != inst_index))
                needs_reset = true;
        }
    }

    if (!already_contained || needs_reset) {
        wxGetApp().plater()->take_snapshot(std::string("Selection-Add!"), UndoRedo::SnapshotType::Selection);

        if (needs_reset)
            clear();

        // BBS
        if (!keep_instance_mode)
            m_mode = volume->is_modifier ? Volume : m_volume_selection_mode;
    }
    else
      // keep current mode
      return;

    switch (m_mode)
    {
    case Volume:
    {
        if (volume->volume_idx() >= 0 && (is_empty() || volume->instance_idx() == get_instance_idx()))
            do_add_volume(volume_idx);

        break;
    }
    case Instance:
    {
        Plater::SuppressSnapshots suppress(wxGetApp().plater());
        add_instance(volume->object_idx(), volume->instance_idx(), as_single_selection);
        break;
    }
    }

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::remove(unsigned int volume_idx)
{
    if (!m_valid || (unsigned int)m_volumes->size() <= volume_idx)
        return;

    if (!contains_volume(volume_idx))
        return;

    wxGetApp().plater()->take_snapshot(std::string("Selection-Remove!"), UndoRedo::SnapshotType::Selection);

    GLVolume* volume = (*m_volumes)[volume_idx];

    switch (m_mode)
    {
    case Volume:
    {
        do_remove_volume(volume_idx);
        break;
    }
    case Instance:
    {
        do_remove_instance(volume->object_idx(), volume->instance_idx());
        break;
    }
    }

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::add_object(unsigned int object_idx, bool as_single_selection)
{
    if (!m_valid)
        return;

    std::vector<unsigned int> volume_idxs = get_volume_idxs_from_object(object_idx);
    if ((!as_single_selection && contains_all_volumes(volume_idxs)) ||
        (as_single_selection && matches(volume_idxs)))
        return;

    wxGetApp().plater()->take_snapshot(std::string("Selection-Add Object"), UndoRedo::SnapshotType::Selection);

    // resets the current list if needed
    if (as_single_selection)
        clear();

    m_mode = Instance;

    do_add_volumes(volume_idxs);

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::remove_object(unsigned int object_idx)
{
    if (!m_valid)
        return;

    wxGetApp().plater()->take_snapshot(std::string("Selection-Remove Object"), UndoRedo::SnapshotType::Selection);

    do_remove_object(object_idx);

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::add_instance(unsigned int object_idx, unsigned int instance_idx, bool as_single_selection)
{
    if (!m_valid)
        return;

    const std::vector<unsigned int> volume_idxs = get_volume_idxs_from_instance(object_idx, instance_idx);
    if ((!as_single_selection && contains_all_volumes(volume_idxs)) ||
        (as_single_selection && matches(volume_idxs)))
        return;

    wxGetApp().plater()->take_snapshot(std::string("Selection-Add Instance"), UndoRedo::SnapshotType::Selection);

    // resets the current list if needed
    if (as_single_selection)
        clear();

    m_mode = Instance;

    do_add_volumes(volume_idxs);

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::remove_instance(unsigned int object_idx, unsigned int instance_idx)
{
    if (!m_valid)
        return;

    wxGetApp().plater()->take_snapshot(std::string("Selection-Remove Instance"), UndoRedo::SnapshotType::Selection);

    do_remove_instance(object_idx, instance_idx);

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::add_volume(unsigned int object_idx, unsigned int volume_idx, int instance_idx, bool as_single_selection)
{
    if (!m_valid)
        return;

    std::vector<unsigned int> volume_idxs = get_volume_idxs_from_volume(object_idx, instance_idx, volume_idx);
    if ((!as_single_selection && contains_all_volumes(volume_idxs)) ||
        (as_single_selection && matches(volume_idxs)))
        return;

    // resets the current list if needed
    if (as_single_selection)
        clear();

    m_mode = Volume;

    do_add_volumes(volume_idxs);

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::remove_volume(unsigned int object_idx, unsigned int volume_idx)
{
    if (!m_valid)
        return;

    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i) {
        GLVolume* v = (*m_volumes)[i];
        if (v->object_idx() == (int)object_idx && v->volume_idx() == (int)volume_idx)
            do_remove_volume(i);
    }

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::add_volumes(EMode mode, const std::vector<unsigned int>& volume_idxs, bool as_single_selection)
{
    if (!m_valid)
        return;

    if ((!as_single_selection && contains_all_volumes(volume_idxs)) ||
        (as_single_selection && matches(volume_idxs)))
        return;

    // resets the current list if needed
    if (as_single_selection)
        clear();

    m_mode = mode;
    for (unsigned int i : volume_idxs) {
        if (i < (unsigned int)m_volumes->size())
            do_add_volume(i);
    }

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::remove_volumes(EMode mode, const std::vector<unsigned int>& volume_idxs)
{
    if (!m_valid)
        return;

    m_mode = mode;
    for (unsigned int i : volume_idxs) {
        if (i < (unsigned int)m_volumes->size())
            do_remove_volume(i);
    }

    update_type();
    this->set_bounding_boxes_dirty();
}

ModelVolume *Selection::get_selected_single_volume(int &out_object_idx, int &out_volume_idx)
{
    if (is_single_volume() || is_single_modifier()) {
        const GLVolume *gl_volume = get_volume(*get_volume_idxs().begin());
        out_object_idx            = gl_volume->object_idx();
        ModelObject *model_object = get_model()->objects[out_object_idx];
        out_volume_idx            = gl_volume->volume_idx();
        if (out_volume_idx < model_object->volumes.size())
            return model_object->volumes[out_volume_idx];
    }
    return nullptr;
}

ModelObject *Selection::get_selected_single_object(int &out_object_idx)
{
    if (is_single_volume() || is_single_modifier() || is_single_full_object()) {
        const GLVolume *gl_volume = get_volume(*get_volume_idxs().begin());
        out_object_idx            = gl_volume->object_idx();
        return get_model()->objects[out_object_idx];
    }
    return nullptr;
}

const std::vector<Transform3d> &Selection::get_all_tran_of_selected_volumes()
{
    m_trafo_matrices.clear();
    int  object_idx;
    auto mo = get_selected_single_object(object_idx);
    if (mo) {
        const ModelInstance *mi = mo->instances[get_instance_idx()];
        for (const ModelVolume *mv : mo->volumes) {
            if (mv->is_model_part()) {
                m_trafo_matrices.emplace_back(mi->get_transformation().get_matrix() * mv->get_matrix());
            }
        }
    }
    return m_trafo_matrices;
}

const ModelInstance *Selection::get_selected_single_intance()
{
    int  object_idx;
    auto mo = get_selected_single_object(object_idx);
    if (mo) {
        return mo->instances[get_instance_idx()];
    }
    return nullptr;
}

void Selection::add_curr_plate()
{
    if (!m_valid)
        return;

    wxGetApp().plater()->take_snapshot(std::string("Selection-Add Curr Plate All!"));
    m_mode = Instance;
    clear();

    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    for (int obj_idx = 0; obj_idx < m_model->objects.size(); obj_idx++) {
        if (plate && plate->contain_instance_totally(obj_idx, 0)) {
            std::vector<unsigned int> volume_idxs = get_volume_idxs_from_object(obj_idx);
            do_add_volumes(volume_idxs);
        }
    }

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::add_object_from_idx(std::vector<int>& object_idxs) {
    if (!m_valid)
        return;

    m_mode = Instance;
    clear();

    for (int obj_idx = 0; obj_idx < object_idxs.size(); obj_idx++) {
        std::vector<unsigned int> volume_idxs = get_volume_idxs_from_object(object_idxs[obj_idx]);
        do_add_volumes(volume_idxs);
    }

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::remove_curr_plate()
{
    if (!m_valid)
        return;

    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    if (plate->empty())
        return;

    wxGetApp().plater()->take_snapshot(std::string("Selection-Delete Curr Plate All"));
    m_mode = Instance;
    clear();

    for (int obj_idx = 0; obj_idx < m_model->objects.size(); obj_idx++) {
        if (plate && plate->contain_instance(obj_idx, 0)) {
            std::vector<unsigned int> volume_idxs = get_volume_idxs_from_object(obj_idx);
            do_add_volumes(volume_idxs);
        }
    }

    update_type();
    this->set_bounding_boxes_dirty();

    erase();
}

void Selection::clone(int numbers)
{
    if (numbers <= 0)
        return;

    wxGetApp().plater()->take_snapshot(std::string("Selection-clone"));
    copy_to_clipboard();
    for (int i = 0; i < numbers; i++) {
        paste_from_clipboard();
    }
}

void Selection::center()
{
    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_selected_plate();

    // calc distance
    Vec3d src_pos = this->get_bounding_box().center();
    Vec3d tar_pos = plate->get_center_origin();
    Vec3d distance = Vec3d(tar_pos.x() - src_pos.x(), tar_pos.y() - src_pos.y(), 0);

    this->move_to_center(distance);
    wxGetApp().plater()->get_view3D_canvas3D()->do_move(L("Move Object"));
    return;
}

void Selection::center_plate(const int plate_idx) {

    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_plate(plate_idx);


    Vec3d src_pos = this->get_bounding_box().center();
    Vec3d tar_pos = plate->get_center_origin();
    Vec3d distance = Vec3d(tar_pos.x() - src_pos.x(), tar_pos.y() - src_pos.y(), 0);

    this->move_to_center(distance);
    wxGetApp().plater()->get_view3D_canvas3D()->do_move(L("Move Object"));
    return;
}

//BBS
void Selection::set_printable(bool printable)
{
    if (!m_valid)
        return;

    std::set<std::pair<int, int>> instances_idxs;
    for (ObjectIdxsToInstanceIdxsMap::iterator obj_it = m_cache.content.begin(); obj_it != m_cache.content.end(); ++obj_it)
    {
        for (InstanceIdxsList::reverse_iterator inst_it = obj_it->second.rbegin(); inst_it != obj_it->second.rend(); ++inst_it)
        {
            instances_idxs.insert(std::make_pair(obj_it->first, *inst_it));
        }
    }

    std::string snapshot_text = (boost::format("%1%") % (printable ? "Set Selection Printable" : "Set Selection Unprintable")).str();
    wxGetApp().plater()->take_snapshot(snapshot_text);

    // set printable value for all instances in object
    for (const std::pair<int, int>& i : instances_idxs)
    {
        ModelObject* object = m_model->objects[i.first];
        for (auto inst : object->instances)
            inst->printable = printable;
        wxGetApp().obj_list()->update_printable_state(i.first, i.second);

        //update printable state on canvas
        wxGetApp().plater()->canvas3D()->update_instance_printable_state_for_object((size_t)i.first);
    }

    // update scene
    wxGetApp().plater()->update();
}

void Selection::add_all()
{
    if (!m_valid)
        return;

    unsigned int count = 0;
    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i) {
        if (!(*m_volumes)[i]->is_wipe_tower)
            ++count;
    }

    if ((unsigned int)m_list.size() == count)
        return;

    wxGetApp().plater()->take_snapshot(std::string("Selection-Add All!"), UndoRedo::SnapshotType::Selection);

    m_mode = Instance;
    clear();

    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i) {
        if (!(*m_volumes)[i]->is_wipe_tower)
            do_add_volume(i);
    }

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::remove_all()
{
    if (!m_valid)
        return;

    if (is_empty())
        return;

// Not taking the snapshot with non-empty Redo stack will likely be more confusing than losing the Redo stack.
// Let's wait for user feedback.
//    if (!wxGetApp().plater()->can_redo())
        wxGetApp().plater()->take_snapshot(std::string("Selection-Remove All!"), UndoRedo::SnapshotType::Selection);

    m_mode = Instance;
    clear();
}

void Selection::set_deserialized(EMode mode, const std::vector<std::pair<size_t, size_t>> &volumes_and_instances)
{
    if (! m_valid)
        return;

    m_mode = mode;
    for (unsigned int i : m_list)
        (*m_volumes)[i]->selected = false;
    m_list.clear();
    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++ i)
		if (std::binary_search(volumes_and_instances.begin(), volumes_and_instances.end(), (*m_volumes)[i]->geometry_id))
			do_add_volume(i);
    update_type();
    set_bounding_boxes_dirty();
}

void Selection::clear()
{
    if (!m_valid)
        return;

    if (m_list.empty())
        return;

#if ENABLE_MODIFIERS_ALWAYS_TRANSPARENT
    // ensure that the volumes get the proper color before next call to render (expecially needed for transparent volumes)
    for (unsigned int i : m_list) {
        GLVolume& volume = *(*m_volumes)[i];
        volume.selected = false;
        bool transparent = volume.color[3] < 1.0f;
        if (transparent)
            volume.force_transparent = true;
        volume.set_render_color();
        if (transparent)
            volume.force_transparent = false;
    }
#else
    for (unsigned int i : m_list) {
        (*m_volumes)[i]->selected = false;
        // ensure the volume gets the proper color before next call to render (expecially needed for transparent volumes)
        (*m_volumes)[i]->set_render_color();
    }
#endif // ENABLE_MODIFIERS_ALWAYS_TRANSPARENT

    m_list.clear();

    update_type();
    set_bounding_boxes_dirty();

    // BBS
#if 0
    // this happens while the application is closing
    if (wxGetApp().obj_manipul() == nullptr)
        return;

    // resets the cache in the sidebar
    wxGetApp().obj_manipul()->reset_cache();
#endif

    // #et_FIXME fake KillFocus from sidebar
    wxGetApp().plater()->canvas3D()->handle_sidebar_focus_event("", false);
}

// Update the selection based on the new instance IDs.
void Selection::instances_changed(const std::vector<size_t> &instance_ids_selected)
{
    assert(m_valid);
    assert(m_mode == Instance);
    m_list.clear();
    for (unsigned int volume_idx = 0; volume_idx < (unsigned int)m_volumes->size(); ++ volume_idx) {
        const GLVolume *volume = (*m_volumes)[volume_idx];
        auto it = std::lower_bound(instance_ids_selected.begin(), instance_ids_selected.end(), volume->geometry_id.second);
		if (it != instance_ids_selected.end() && *it == volume->geometry_id.second)
            this->do_add_volume(volume_idx);
    }
    update_type();
    this->set_bounding_boxes_dirty();
}

// Update the selection based on the map from old indices to new indices after m_volumes changed.
// If the current selection is by instance, this call may select newly added volumes, if they belong to already selected instances.
void Selection::volumes_changed(const std::vector<size_t> &map_volume_old_to_new)
{
    assert(m_valid);
    assert(m_mode == Volume);
    IndicesList list_new;
    for (unsigned int idx : m_list)
        if (map_volume_old_to_new[idx] != size_t(-1)) {
            unsigned int new_idx = (unsigned int)map_volume_old_to_new[idx];
            (*m_volumes)[new_idx]->selected = true;
            list_new.insert(new_idx);
        }
    m_list = std::move(list_new);
    update_type();
    this->set_bounding_boxes_dirty();
}

bool Selection::is_single_full_instance() const
{
    if (m_type == SingleFullInstance)
        return true;

    if (m_type == SingleFullObject)
        return get_instance_idx() != -1;

    if (m_list.empty() || m_volumes->empty())
        return false;

    int object_idx = m_valid ? get_object_idx() : -1;
    if (object_idx < 0 || (int)m_model->objects.size() <= object_idx)
        return false;

    int instance_idx = (*m_volumes)[*m_list.begin()]->instance_idx();

    std::set<int> volumes_idxs;
    for (unsigned int i : m_list) {
        const GLVolume* v = (*m_volumes)[i];
        if (object_idx != v->object_idx() || instance_idx != v->instance_idx())
            return false;

        int volume_idx = v->volume_idx();
        if (volume_idx >= 0)
            volumes_idxs.insert(volume_idx);
    }

    return m_model->objects[object_idx]->volumes.size() == volumes_idxs.size();
}

bool Selection::is_any_connector() const
{
    const int obj_idx = get_object_idx();

    if ((is_any_volume() || is_any_modifier() || is_mixed()) && // some solid_part AND/OR modifier is selected
        obj_idx >= 0 && m_model->objects[obj_idx]->is_cut()) {
        const ModelVolumePtrs &obj_volumes = m_model->objects[obj_idx]->volumes;
        for (size_t vol_idx = 0; vol_idx < obj_volumes.size(); vol_idx++)
            if (obj_volumes[vol_idx]->is_cut_connector())
                for (const GLVolume *v : *m_volumes)
                    if (v->object_idx() == obj_idx && v->volume_idx() == (int) vol_idx && v->selected)
                        return true;
    }
    return false;
}

bool Selection::is_any_cut_volume() const
{
    const int obj_idx = get_object_idx();
    return is_any_volume() && obj_idx >= 0 && m_model->objects[obj_idx]->is_cut();
}

bool Selection::is_from_single_object() const
{
    const int idx = get_object_idx();
    return 0 <= idx && idx < 1000;
}

bool Selection::is_sla_compliant() const
{
    if (m_mode == Volume)
        return false;

    for (unsigned int i : m_list) {
        if ((*m_volumes)[i]->is_modifier)
            return false;
    }

    return true;
}

bool Selection::has_emboss_shape() const
{
    if (!m_model) {
        return false;
    }

    const int obj_idx = get_object_idx();
    if (obj_idx < 0 || obj_idx >= m_model->objects.size()) {
        return false;
    }

    const ModelVolumePtrs& obj_volumes = m_model->objects[obj_idx]->volumes;
    for (size_t vol_idx = 0; vol_idx < obj_volumes.size(); vol_idx++) {
        if (obj_volumes[vol_idx] && obj_volumes[vol_idx]->emboss_shape.has_value()) {
            return true;
        }
    }
    return false;
}

bool Selection::contains_all_volumes(const std::vector<unsigned int>& volume_idxs) const
{
    for (unsigned int i : volume_idxs) {
        if (m_list.find(i) == m_list.end())
            return false;
    }

    return true;
}

bool Selection::contains_any_volume(const std::vector<unsigned int>& volume_idxs) const
{
    for (unsigned int i : volume_idxs) {
        if (m_list.find(i) != m_list.end())
            return true;
    }

    return false;
}

bool Selection::matches(const std::vector<unsigned int>& volume_idxs) const
{
    unsigned int count = 0;

    for (unsigned int i : volume_idxs) {
        if (m_list.find(i) != m_list.end())
            ++count;
        else
            return false;
    }

    return count == (unsigned int)m_list.size();
}

bool Selection::requires_uniform_scale() const
{
    if (is_single_full_instance() || is_single_modifier() || is_single_volume())
        return false;

    return true;
}

int Selection::get_object_idx() const
{
    return (m_cache.content.size() == 1) ? m_cache.content.begin()->first : -1;
}

int Selection::get_instance_idx() const
{
    if (m_cache.content.size() == 1) {
        const InstanceIdxsList& idxs = m_cache.content.begin()->second;
        if (idxs.size() == 1)
            return *idxs.begin();
    }

    return -1;
}

const Selection::InstanceIdxsList& Selection::get_instance_idxs() const
{
    assert(m_cache.content.size() == 1);
    return m_cache.content.begin()->second;
}

const GLVolume* Selection::get_volume(unsigned int volume_idx) const
{
    return (m_valid && (volume_idx < (unsigned int)m_volumes->size())) ? (*m_volumes)[volume_idx] : nullptr;
}

GLVolume *Selection::get_volume(unsigned int volume_idx) {
    return (m_valid && (volume_idx < (unsigned int) m_volumes->size())) ? (*m_volumes)[volume_idx] : nullptr;
}


const GLVolume *Selection::get_volume_by_object_volumn_id(unsigned int volume_id) const
{
    if (!m_valid || m_volumes->size() <= 0)
        return nullptr;
    for (const GLVolume *v : *m_volumes) {
        if (v->object_idx() == get_object_idx() && v->volume_idx() == volume_id)
            return const_cast<GLVolume *>(v);
    }
    return nullptr;
}

const GLVolume* Selection::get_first_volume() const
{
    if (!m_list.size()) {
        return nullptr;
    }
    return get_volume(*m_list.begin());
}

const BoundingBoxf3& Selection::get_bounding_box() const
{
    if (!m_bounding_box.has_value()) {
        std::optional<BoundingBoxf3>* bbox = const_cast<std::optional<BoundingBoxf3>*>(&m_bounding_box);
        *bbox = BoundingBoxf3();
        if (m_valid) {
            for (unsigned int i : m_list) {
                (*bbox)->merge((*m_volumes)[i]->transformed_convex_hull_bounding_box());
            }
        }
    }
    return *m_bounding_box;
}

const BoundingBoxf3& Selection::get_unscaled_instance_bounding_box() const
{
    if (!m_unscaled_instance_bounding_box.has_value()) {
        std::optional<BoundingBoxf3>* bbox = const_cast<std::optional<BoundingBoxf3>*>(&m_unscaled_instance_bounding_box);
        *bbox = BoundingBoxf3();
        if (m_valid) {
            for (unsigned int i : m_list) {
                const GLVolume& volume = *(*m_volumes)[i];
                if (volume.is_modifier)
                    continue;
                Transform3d trafo = volume.get_instance_transformation().get_matrix(false, false, true, false) * volume.get_volume_transformation().get_matrix();
                trafo.translation().z() += volume.get_sla_shift_z();
                (*bbox)->merge(volume.transformed_convex_hull_bounding_box(trafo));
            }
        }
    }
    return *m_unscaled_instance_bounding_box;
}

const BoundingBoxf3& Selection::get_scaled_instance_bounding_box() const
{
    if (!m_scaled_instance_bounding_box.has_value()) {
        std::optional<BoundingBoxf3>* bbox = const_cast<std::optional<BoundingBoxf3>*>(&m_scaled_instance_bounding_box);
        *bbox = BoundingBoxf3();
        if (m_valid) {
            for (unsigned int i : m_list) {
                const GLVolume& volume = *(*m_volumes)[i];
                if (volume.is_modifier)
                    continue;
                Transform3d trafo = volume.get_instance_transformation().get_matrix(false, false, false, false) * volume.get_volume_transformation().get_matrix();
                trafo.translation().z() += volume.get_sla_shift_z();
                (*bbox)->merge(volume.transformed_convex_hull_bounding_box(trafo));
            }
        }
    }
    return *m_scaled_instance_bounding_box;
}

const BoundingBoxf3 &Selection::get_full_unscaled_instance_bounding_box() const
{
    assert(is_single_full_instance());

    if (!m_full_unscaled_instance_bounding_box.has_value()) {
        std::optional<BoundingBoxf3> *bbox = const_cast<std::optional<BoundingBoxf3> *>(&m_full_unscaled_instance_bounding_box);
        *bbox                              = BoundingBoxf3();
        if (m_valid) {
            for (unsigned int i : m_list) {
                const GLVolume &volume = *(*m_volumes)[i];
                Transform3d     trafo  = volume.get_instance_transformation().get_matrix_no_scaling_factor() * volume.get_volume_transformation().get_matrix();
                trafo.translation().z() += volume.get_sla_shift_z();
                (*bbox)->merge(volume.transformed_convex_hull_bounding_box(trafo));
            }
        }
    }
    return *m_full_unscaled_instance_bounding_box;
}

const BoundingBoxf3 &Selection::get_full_scaled_instance_bounding_box() const
{
    assert(is_single_full_instance());

    if (!m_full_scaled_instance_bounding_box.has_value()) {
        std::optional<BoundingBoxf3> *bbox = const_cast<std::optional<BoundingBoxf3> *>(&m_full_scaled_instance_bounding_box);
        *bbox                              = BoundingBoxf3();
        if (m_valid) {
            for (unsigned int i : m_list) {
                const GLVolume &volume = *(*m_volumes)[i];
                Transform3d     trafo  = volume.get_instance_transformation().get_matrix() * volume.get_volume_transformation().get_matrix();
                trafo.translation().z() += volume.get_sla_shift_z();
                (*bbox)->merge(volume.transformed_convex_hull_bounding_box(trafo));
            }
        }
    }
    return *m_full_scaled_instance_bounding_box;
}

const BoundingBoxf3 &Selection::get_full_unscaled_instance_local_bounding_box() const
{
    assert(is_single_full_instance());

    if (!m_full_unscaled_instance_local_bounding_box.has_value()) {
        std::optional<BoundingBoxf3> *bbox = const_cast<std::optional<BoundingBoxf3> *>(&m_full_unscaled_instance_local_bounding_box);
        *bbox                              = BoundingBoxf3();
        if (m_valid) {
            for (unsigned int i : m_list) {
                const GLVolume &volume = *(*m_volumes)[i];
                Transform3d     trafo  = volume.get_volume_transformation().get_matrix();
                trafo.translation().z() += volume.get_sla_shift_z();
                (*bbox)->merge(volume.transformed_convex_hull_bounding_box(trafo));
            }
        }
    }
    return *m_full_unscaled_instance_local_bounding_box;
}

const std::pair<BoundingBoxf3, Transform3d> &Selection::get_bounding_box_in_current_reference_system() const
{
    static int last_coordinates_type = -1;

    assert(!is_empty());

    ECoordinatesType coordinates_type = wxGetApp().obj_manipul()->get_coordinates_type();
    if (m_mode == Instance && coordinates_type == ECoordinatesType::Local)
        coordinates_type = ECoordinatesType::World;

    if (last_coordinates_type != int(coordinates_type))
        const_cast<std::optional<std::pair<BoundingBoxf3, Transform3d>> *>(&m_bounding_box_in_current_reference_system)->reset();

    if (!m_bounding_box_in_current_reference_system.has_value()) {
        last_coordinates_type                                                                                            = int(coordinates_type);
        *const_cast<std::optional<std::pair<BoundingBoxf3, Transform3d>> *>(&m_bounding_box_in_current_reference_system) = get_bounding_box_in_reference_system(coordinates_type);
    }

    return *m_bounding_box_in_current_reference_system;
}

std::pair<BoundingBoxf3, Transform3d> Selection::get_bounding_box_in_reference_system(ECoordinatesType type) const
{
    //
    // trafo to current reference system
    //
    Transform3d trafo;
    switch (type) {
    case ECoordinatesType::World: {
        trafo = Transform3d::Identity();
        break;
    }
    case ECoordinatesType::Instance: {
        trafo = get_first_volume()->get_instance_transformation().get_matrix();
        break;
    }
    case ECoordinatesType::Local: {
        trafo = get_first_volume()->world_matrix();
        break;
    }
    }

    //
    // trafo basis in world coordinates
    //
    Geometry::Transformation t(trafo);
    t.reset_scaling_factor();
    const Transform3d  basis_trafo = t.get_matrix_no_offset();
    std::vector<Vec3d> axes        = {Vec3d::UnitX(), Vec3d::UnitY(), Vec3d::UnitZ()};
    for (size_t i = 0; i < axes.size(); ++i) { axes[i] = basis_trafo * axes[i]; }

    //
    // calculate bounding box aligned to trafo basis
    //
    Vec3d min = {DBL_MAX, DBL_MAX, DBL_MAX};
    Vec3d max = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
    for (unsigned int id : m_list) {
        const GLVolume &    vol            = *get_volume(id);
        const Transform3d   vol_world_rafo = vol.world_matrix();
        const TriangleMesh *mesh           = vol.convex_hull();
        if (mesh == nullptr)
            mesh = &m_model->objects[vol.object_idx()]->volumes[vol.volume_idx()]->mesh();
        assert(mesh != nullptr);
        for (const stl_vertex &v : mesh->its.vertices) {
            const Vec3d world_v = vol_world_rafo * v.cast<double>();
            for (int i = 0; i < 3; ++i) {
                const double i_comp = world_v.dot(axes[i]);
                min(i)              = std::min(min(i), i_comp);
                max(i)              = std::max(max(i), i_comp);
            }
        }
    }

    const Vec3d              box_size      = max - min;
    Vec3d                    half_box_size = 0.5 * box_size;
    Geometry::Transformation out_trafo(trafo);
    Vec3d                    center = 0.5 * (min + max);

    // Fix for non centered volume
    // by move with calculated center(to volume center) and extend half box size
    // e.g. for right aligned embossed text
    if (m_list.size() == 1 && type == ECoordinatesType::Local) {
        const GLVolume &  vol             = *get_volume(*m_list.begin());
        bool condition = !vol.is_text_shape;
        if (condition){
            const Transform3d vol_world_trafo = vol.world_matrix();
            Vec3d             world_zero      = vol_world_trafo * Vec3d::Zero();
            for (size_t i = 0; i < 3; i++) {
                // move center to local volume zero
                center[i] = world_zero.dot(axes[i]);
                // extend half size to bigger distance from center
                half_box_size[i] = std::max(abs(center[i] - min[i]), abs(center[i] - max[i]));
            }
        }
    }

    const BoundingBoxf3 out_box(-half_box_size, half_box_size);
    out_trafo.set_offset(basis_trafo * center);
    return {out_box, out_trafo.get_matrix_no_scaling_factor()};
}

void Selection::start_dragging()
{
    if (!m_valid)
        return;

    m_dragging = true;
    set_caches();
}

void Selection::move_to_center(const Vec3d& displacement, bool local)
{
    if (!m_valid)
        return;

    EMode translation_type = m_mode;
    //BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": %1%, displacement {%2%, %3%, %4%}") % __LINE__ % displacement(X) % displacement(Y) % displacement(Z);

    set_caches();
    for (unsigned int i : m_list) {
        GLVolume& v = *(*m_volumes)[i];
        if (m_mode == Volume) {
            if (local)
                v.set_volume_offset(m_cache.volumes_data[i].get_volume_position() + displacement);
            else {
                const Vec3d local_displacement = (m_cache.volumes_data[i].get_instance_rotation_matrix() * m_cache.volumes_data[i].get_instance_scale_matrix() * m_cache.volumes_data[i].get_instance_mirror_matrix()).inverse() * displacement;
                v.set_volume_offset(m_cache.volumes_data[i].get_volume_position() + local_displacement);
            }
        }
        else if (m_mode == Instance) {
            if (is_from_fully_selected_instance(i)) {
                v.set_instance_offset(m_cache.volumes_data[i].get_instance_position() + displacement);
            }
            else {
                const Vec3d local_displacement = (m_cache.volumes_data[i].get_instance_rotation_matrix() * m_cache.volumes_data[i].get_instance_scale_matrix() * m_cache.volumes_data[i].get_instance_mirror_matrix()).inverse() * displacement;
                v.set_volume_offset(m_cache.volumes_data[i].get_volume_position() + local_displacement);
                translation_type = Volume;
            }
        }
    }
    this->set_bounding_boxes_dirty();
}

const std::pair<Vec3d, double> Selection::get_bounding_sphere() const
{
    if (!m_bounding_sphere.has_value()) {
        std::optional<std::pair<Vec3d, double>> *sphere = const_cast<std::optional<std::pair<Vec3d, double>> *>(&m_bounding_sphere);
        *sphere                                         = {Vec3d::Zero(), 0.0};

        using K          = CGAL::Simple_cartesian<float>;
        using Traits     = CGAL::Min_sphere_of_points_d_traits_3<K, float>;
        using Min_sphere = CGAL::Min_sphere_of_spheres_d<Traits>;
        using Point      = K::Point_3;

        std::vector<Point> points;
        if (m_valid) {
            for (unsigned int i : m_list) {
                const GLVolume &            volume = *(*m_volumes)[i];
                const TriangleMesh *        hull   = volume.convex_hull();
                const indexed_triangle_set &its    = (hull != nullptr) ? hull->its : m_model->objects[volume.object_idx()]->volumes[volume.volume_idx()]->mesh().its;
                const Transform3d &         matrix = volume.world_matrix();
                for (const Vec3f &v : its.vertices) {
                    const Vec3d vv = matrix * v.cast<double>();
                    points.push_back(Point(vv.x(), vv.y(), vv.z()));
                }
            }

            if (points.size() > 0) {
                Min_sphere   ms(points.begin(), points.end());
                const float* center_x = ms.center_cartesian_begin();
                (*sphere)->first = { *center_x, *(center_x + 1), *(center_x + 2) };
                (*sphere)->second = ms.radius();
            }
        }
    }

    return *m_bounding_sphere;
}

void Selection::setup_cache()
{
    if (!m_valid)
        return;
    set_caches();
}

void Selection::translate(const Vec3d &displacement, TransformationType transformation_type)
{
    if (!m_valid) return;

    for (unsigned int i : m_list) {
        GLVolume &         v           = *(*m_volumes)[i];
        const VolumeCache &volume_data = m_cache.volumes_data[i];
        if (m_mode == Instance && !is_wipe_tower()) {
            assert(is_from_fully_selected_instance(i));
            if (transformation_type.instance()) {
                const Geometry::Transformation &inst_trafo = volume_data.get_instance_transform();
                v.set_instance_offset(inst_trafo.get_offset() + inst_trafo.get_rotation_matrix() * displacement);
            } else
                transform_instance_relative(v, volume_data, transformation_type, Geometry::translation_transform(displacement), m_cache.dragging_center);
        } else {
            if (v.is_wipe_tower) {//in world cs
                int           plate_idx           = v.object_idx() - 1000;
                BoundingBoxf3 plate_bbox = wxGetApp().plater()->get_partplate_list().get_plate(plate_idx)->get_build_volume(true);
                BoundingBox   plate_bbox2d        = BoundingBox(scaled(Vec2f(plate_bbox.min[0], plate_bbox.min[1])), scaled(Vec2f(plate_bbox.max[0], plate_bbox.max[1])));
                Vec3d         tower_size          = v.bounding_box().size();
                Vec3d         tower_origin        = m_cache.volumes_data[i].get_volume_position();
                Vec3d         actual_displacement = displacement;
                bool show_read_wipe_tower = wxGetApp().plater()->get_partplate_list().get_plate(plate_idx)->fff_print()->is_step_done(psWipeTower);

                const double margin = show_read_wipe_tower ? WIPE_TOWER_MARGIN_AFTER_SLICING : WIPE_TOWER_MARGIN;

                actual_displacement = (m_cache.volumes_data[i].get_instance_rotation_matrix() * m_cache.volumes_data[i].get_instance_scale_matrix() *
                                        m_cache.volumes_data[i].get_instance_mirror_matrix())
                                            .inverse() *
                                        displacement;
                BoundingBoxf3 tower_bbox = v.bounding_box();
                tower_bbox.translate(actual_displacement + tower_origin);
                BoundingBox   tower_bbox2d = BoundingBox(scaled(Vec2f(tower_bbox.min[0], tower_bbox.min[1])), scaled(Vec2f(tower_bbox.max[0], tower_bbox.max[1])));
                Vec2f offset = WipeTower::move_box_inside_box(tower_bbox2d, plate_bbox2d,scaled(margin));
                //if (tower_origin(0) + actual_displacement(0) - margin < plate_bbox.min(0)) {
                //    actual_displacement(0) = plate_bbox.min(0) - tower_origin(0) + margin;
                //} else if (tower_origin(0) + actual_displacement(0) + tower_size(0) + margin > plate_bbox.max(0)) {
                //    actual_displacement(0) = plate_bbox.max(0) - tower_origin(0) - tower_size(0) - margin;
                //}

                //if (tower_origin(1) + actual_displacement(1) - margin < plate_bbox.min(1)) {
                //    actual_displacement(1) = plate_bbox.min(1) - tower_origin(1) + margin;
                //} else if (tower_origin(1) + actual_displacement(1) + tower_size(1) + margin > plate_bbox.max(1)) {
                //    actual_displacement(1) = plate_bbox.max(1) - tower_origin(1) - tower_size(1) - margin;
                //}
                actual_displacement += Vec3d(offset[0], offset[1],0);
                v.set_volume_offset(m_cache.volumes_data[i].get_volume_position() + actual_displacement);
            }
            else if (transformation_type.local() && transformation_type.absolute()) {
                const Geometry::Transformation &vol_trafo  = volume_data.get_volume_transform();
                const Geometry::Transformation &inst_trafo = volume_data.get_instance_transform();
                v.set_volume_offset(vol_trafo.get_offset() + inst_trafo.get_scaling_factor_matrix().inverse() * vol_trafo.get_rotation_matrix() * displacement);
            } else {
                Vec3d relative_disp = displacement;
                if (transformation_type.world() && transformation_type.instance())
                    relative_disp = volume_data.get_instance_transform().get_scaling_factor_matrix().inverse() * relative_disp;

                transform_volume_relative(v, volume_data, transformation_type, Geometry::translation_transform(relative_disp), m_cache.dragging_center);
            }
        }
    }

#if !DISABLE_INSTANCES_SYNCH
    if (m_mode == Instance)
        synchronize_unselected_instances(SyncRotationType::NONE);
    else if (m_mode == Volume)
        synchronize_unselected_volumes();
#endif // !DISABLE_INSTANCES_SYNCH
    if (wxGetApp().plater()->canvas3D()->get_canvas_type() != GLCanvas3D::ECanvasType::CanvasAssembleView) {
        ensure_not_below_bed();
    }
    set_bounding_boxes_dirty();
    if (wxGetApp().plater()->canvas3D()->get_canvas_type() != GLCanvas3D::ECanvasType::CanvasAssembleView) {
        wxGetApp().plater()->canvas3D()->requires_check_outside_state();
    }
}
// Rotate an object around one of the axes. Only one rotation component is expected to be changing.
void Selection::rotate(const Vec3d& rotation, TransformationType transformation_type)
{
    if (!m_valid)
        return;

    // Only relative rotation values are allowed in the world coordinate system.
    assert(!transformation_type.world() || transformation_type.relative());

    if (!is_wipe_tower()) {
        int rot_axis_max = 0;
        if (rotation.isApprox(Vec3d::Zero())) {
            for (unsigned int i : m_list) {
                GLVolume &v = *(*m_volumes)[i];
                if (m_mode == Instance) {
                    v.set_instance_rotation(m_cache.volumes_data[i].get_instance_rotation());
                    v.set_instance_offset(m_cache.volumes_data[i].get_instance_position());
                }
                else if (m_mode == Volume) {
                    v.set_volume_rotation(m_cache.volumes_data[i].get_volume_rotation());
                    v.set_volume_offset(m_cache.volumes_data[i].get_volume_position());
                }
            }
        }
        else { // this is not the wipe tower
            //FIXME this does not work for absolute rotations (transformation_type.absolute() is true)
            rotation.cwiseAbs().maxCoeff(&rot_axis_max);

//            if ( single instance or single volume )
                // Rotate around center , if only a single object or volume
//                transformation_type.set_independent();

            // For generic rotation, we want to rotate the first volume in selection, and then to synchronize the other volumes with it.
            std::vector<int> object_instance_first(m_model->objects.size(), -1);
            auto rotate_instance = [this, &rotation, &object_instance_first, rot_axis_max, transformation_type](GLVolume &volume, int i) {
                const int first_volume_idx = object_instance_first[volume.object_idx()];
                if (rot_axis_max != 2 && first_volume_idx != -1) {
                    // Generic rotation, but no rotation around the Z axis.
                    // Always do a local rotation (do not consider the selection to be a rigid body).
                    assert(is_approx(rotation.z(), 0.0));
                    const GLVolume &first_volume = *(*m_volumes)[first_volume_idx];
                    const Vec3d    &rotation = first_volume.get_instance_rotation();
                    const double z_diff = Geometry::rotation_diff_z(m_cache.volumes_data[first_volume_idx].get_instance_rotation(), m_cache.volumes_data[i].get_instance_rotation());
                    volume.set_instance_rotation(Vec3d(rotation(0), rotation(1), rotation(2) + z_diff));
                }
                else {
                    // extracts rotations from the composed transformation
                    Vec3d new_rotation = transformation_type.world() ?
                        Geometry::extract_euler_angles(Geometry::assemble_transform(Vec3d::Zero(), rotation) * m_cache.volumes_data[i].get_instance_rotation_matrix()) :
                        transformation_type.absolute() ? rotation : rotation + m_cache.volumes_data[i].get_instance_rotation();
                    if (rot_axis_max == 2 && transformation_type.joint()) {
                        // Only allow rotation of multiple instances as a single rigid body when rotating around the Z axis.
                        const double z_diff = Geometry::rotation_diff_z(m_cache.volumes_data[i].get_instance_rotation(), new_rotation);
                        volume.set_instance_offset(m_cache.dragging_center + Eigen::AngleAxisd(z_diff, Vec3d::UnitZ()) * (m_cache.volumes_data[i].get_instance_position() - m_cache.dragging_center));
                    }
                    volume.set_instance_rotation(new_rotation);
                    object_instance_first[volume.object_idx()] = i;
                }
            };

            for (unsigned int i : m_list) {
                Transform3d        rotation_matrix = Geometry::rotation_transform(rotation);
                GLVolume &v = *(*m_volumes)[i];
                const VolumeCache &volume_data = m_cache.volumes_data[i];
                const Geometry::Transformation &inst_trafo      = volume_data.get_instance_transform();
                if (m_mode == Instance ||is_single_full_instance()) {
                    assert(is_from_fully_selected_instance(i));
                    if (transformation_type.instance()) {
                        // ensure that the instance rotates as a rigid body
                        Transform3d inst_rotation_matrix = inst_trafo.get_rotation_matrix();
                        if (inst_trafo.is_left_handed()) {
                            Geometry::TransformationSVD inst_svd(inst_trafo);
                            inst_rotation_matrix = inst_svd.u * inst_svd.v.transpose();
                            // ensure the rotation has the proper direction
                            if (!rotation.normalized().cwiseAbs().isApprox(Vec3d::UnitX())) rotation_matrix = rotation_matrix.inverse();
                        }

                        const Transform3d inst_matrix_no_offset = inst_trafo.get_matrix_no_offset();
                        rotation_matrix = inst_matrix_no_offset.inverse() * inst_rotation_matrix * rotation_matrix * inst_rotation_matrix.inverse() * inst_matrix_no_offset;

                        // rotate around selection center
                        const Vec3d inst_pivot = inst_trafo.get_matrix_no_offset().inverse() * (m_cache.rotation_pivot - inst_trafo.get_offset());
                        rotation_matrix        = Geometry::translation_transform(inst_pivot) * rotation_matrix * Geometry::translation_transform(-inst_pivot);
                    }
                    transform_instance_relative(v, volume_data, transformation_type, rotation_matrix, m_cache.rotation_pivot);
                }
                else if (!is_single_volume_or_modifier()) {
                    assert(transformation_type.world());
                    transform_volume_relative(v, volume_data, transformation_type, rotation_matrix, m_cache.rotation_pivot);
                }

                else
                {
                    if (transformation_type.instance()) {//in object Coordinate System
                        // ensure that the volume rotates as a rigid body
                        const Transform3d inst_scale_matrix = inst_trafo.get_scaling_factor_matrix();
                        rotation_matrix                     = inst_scale_matrix.inverse() * rotation_matrix * inst_scale_matrix;
                    } else {
                        if (transformation_type.local()) {
                            // ensure that the volume rotates as a rigid body
                            const Geometry::Transformation &vol_trafo            = volume_data.get_volume_transform();
                            const Transform3d               vol_matrix_no_offset = vol_trafo.get_matrix_no_offset();
                            const Transform3d               inst_scale_matrix    = inst_trafo.get_scaling_factor_matrix();
                            Transform3d                     vol_rotation_matrix  = vol_trafo.get_rotation_matrix();
                            if (vol_trafo.is_left_handed()) {
                                Geometry::TransformationSVD vol_svd(vol_trafo);
                                vol_rotation_matrix = vol_svd.u * vol_svd.v.transpose();
                                // ensure the rotation has the proper direction
                                if (!rotation.normalized().cwiseAbs().isApprox(Vec3d::UnitX())) rotation_matrix = rotation_matrix.inverse();
                            }
                            rotation_matrix = vol_matrix_no_offset.inverse() * inst_scale_matrix.inverse() * vol_rotation_matrix * rotation_matrix *
                                              vol_rotation_matrix.inverse() * inst_scale_matrix * vol_matrix_no_offset;
                        }
                    }
                    transform_volume_relative(v, volume_data, transformation_type, rotation_matrix, m_cache.rotation_pivot);
                }
            }
        }

    #if !DISABLE_INSTANCES_SYNCH
        if (m_mode == Instance)
            synchronize_unselected_instances((rot_axis_max == 2) ? SyncRotationType::NONE : SyncRotationType::GENERAL);
        else if (m_mode == Volume)
            synchronize_unselected_volumes();
    #endif // !DISABLE_INSTANCES_SYNCH
    }
    else { // it's the wipe tower that's selected and being rotated
        GLVolume& volume = *((*m_volumes)[*m_list.begin()]); // the wipe tower is always alone in the selection

        // make sure the wipe tower rotates around its center, not origin
        // we can assume that only Z rotation changes
        const Vec3d center_local = volume.transformed_bounding_box().center() - volume.get_volume_offset();
        const Vec3d center_local_new = Eigen::AngleAxisd(rotation(2)-volume.get_volume_rotation()(2), Vec3d(0.0, 0.0, 1.0)) * center_local;
        volume.set_volume_rotation(rotation);
        volume.set_volume_offset(volume.get_volume_offset() + center_local - center_local_new);
    }

    set_bounding_boxes_dirty();
    if (wxGetApp().plater()->canvas3D()->get_canvas_type() != GLCanvas3D::ECanvasType::CanvasAssembleView) {
        wxGetApp().plater()->canvas3D()->requires_check_outside_state();
    }
}

void Selection::flattening_rotate(const Vec3d& normal)
{
    // We get the normal in untransformed coordinates. We must transform it using the instance matrix, find out
    // how to rotate the instance so it faces downwards and do the rotation. All that for all selected instances.
    // The function assumes that is_from_single_object() holds.
    assert(Slic3r::is_approx(normal.norm(), 1.));

    if (!m_valid)
        return;

    // BBS: show the normal for debug
    std::stringstream ss;
    ss << std::fixed << std::setprecision(4) << ": " << (-normal).transpose();
    wxGetApp().plater()->show_status_message("place on face -normal: "+ss.str());
    BOOST_LOG_TRIVIAL(debug) <<"flattening_rotate at "<<__FILE__<<":"<<__LINE__ << std::fixed << std::setprecision(4) << ": " << normal.transpose();
    flush_logs();

    for (unsigned int i : m_list) {
        GLVolume& v = *(*m_volumes)[i];
        // Normal transformed from the object coordinate space to the world coordinate space.
        const Geometry::Transformation &old_inst_trafo = v.get_instance_transformation();
        const Vec3d                     tnormal        = old_inst_trafo.get_matrix_no_offset() * normal;
        // Additional rotation to align tnormal with the down vector in the world coordinate space.
        const Transform3d rotation_matrix = Transform3d(Eigen::Quaterniond().setFromTwoVectors(tnormal, -Vec3d::UnitZ()));
        v.set_instance_transformation(old_inst_trafo.get_offset_matrix() * rotation_matrix * old_inst_trafo.get_matrix_no_offset());
    }

#if !DISABLE_INSTANCES_SYNCH
    // Apply the same transformation also to other instances,
    // but respect their possibly diffrent z-rotation.
    if (m_mode == Instance)
        synchronize_unselected_instances(SyncRotationType::GENERAL);
#endif // !DISABLE_INSTANCES_SYNCH

    this->set_bounding_boxes_dirty();
}

void Selection::scale(const Vec3d& scale, TransformationType transformation_type)
{
    scale_and_translate(scale, Vec3d::Zero(), transformation_type);
}

#if ENABLE_ENHANCED_PRINT_VOLUME_FIT
void Selection::scale_to_fit_print_volume(const BuildVolume& volume)
{
    auto fit = [this](double s, Vec3d offset) {
        if (s <= 0.0 || s == 1.0)
            return;

        wxGetApp().plater()->take_snapshot(std::string("Scale To Fit"));

        TransformationType type;
        type.set_world();
        type.set_relative();
        type.set_joint();

        // apply scale
        start_dragging();
        scale(s * Vec3d::Ones(), type);
        wxGetApp().plater()->canvas3D()->do_scale(""); // avoid storing another snapshot

        // center selection on print bed
        start_dragging();
        offset.z() = -get_bounding_box().min.z();
        TransformationType trafo_type;
        trafo_type.set_relative();
        translate(offset, trafo_type);
        wxGetApp().plater()->canvas3D()->do_move(""); // avoid storing another snapshot

        // BBS
        //wxGetApp().obj_manipul()->set_dirty();
    };

    auto fit_rectangle = [this, fit](const BuildVolume& volume) {
        const BoundingBoxf3 print_volume = volume.bounding_volume();
        const Vec3d print_volume_size = print_volume.size();

        // adds 1/100th of a mm on all sides to avoid false out of print volume detections due to floating-point roundings
        const Vec3d box_size = get_bounding_box().size() + 0.02 * Vec3d::Ones();

        const double sx = (box_size.x() != 0.0) ? print_volume_size.x() / box_size.x() : 0.0;
        const double sy = (box_size.y() != 0.0) ? print_volume_size.y() / box_size.y() : 0.0;
        const double sz = (box_size.z() != 0.0) ? print_volume_size.z() / box_size.z() : 0.0;

        if (sx != 0.0 && sy != 0.0 && sz != 0.0)
            fit(std::min(sx, std::min(sy, sz)), print_volume.center() - get_bounding_box().center());
    };

    auto fit_circle = [this, fit](const BuildVolume& volume) {
        const Geometry::Circled& print_circle = volume.circle();
        double print_circle_radius = unscale<double>(print_circle.radius);

        if (print_circle_radius == 0.0)
            return;

        Points points;
        double max_z = 0.0;
        for (unsigned int i : m_list) {
            const GLVolume& v = *(*m_volumes)[i];
            TriangleMesh hull_3d = *v.convex_hull();
            hull_3d.transform(v.world_matrix());
            max_z = std::max(max_z, hull_3d.bounding_box().size().z());
            const Polygon hull_2d = hull_3d.convex_hull();
            points.insert(points.end(), hull_2d.begin(), hull_2d.end());
        }

        if (points.empty())
            return;

        const Geometry::Circled circle = Geometry::smallest_enclosing_circle_welzl(points);
        // adds 1/100th of a mm on all sides to avoid false out of print volume detections due to floating-point roundings
        const double circle_radius = unscale<double>(circle.radius) + 0.01;

        if (circle_radius == 0.0 || max_z == 0.0)
            return;

        const double s = std::min(print_circle_radius / circle_radius, volume.printable_height() / max_z);
        const Vec3d sel_center = get_bounding_box().center();
        const Vec3d offset = s * (Vec3d(unscale<double>(circle.center.x()), unscale<double>(circle.center.y()), 0.5 * max_z) - sel_center);
        const Vec3d print_center = { unscale<double>(print_circle.center.x()), unscale<double>(print_circle.center.y()), 0.5 * volume.printable_height() };
        fit(s, print_center - (sel_center + offset));
    };

    if (is_empty() || m_mode == Volume)
        return;

    switch (volume.type())
    {
    case BuildVolume::Type::Rectangle: { fit_rectangle(volume); break; }
    case BuildVolume::Type::Circle:    { fit_circle(volume); break; }
    default: { break; }
    }
}
#else
void Selection::scale_to_fit_print_volume(const DynamicPrintConfig& config)
{
    if (is_empty() || m_mode == Volume)
        return;

    // adds 1/100th of a mm on all sides to avoid false out of print volume detections due to floating-point roundings
    Vec3d box_size = get_bounding_box().size() + 0.01 * Vec3d::Ones();

    const ConfigOptionPoints* opt = dynamic_cast<const ConfigOptionPoints*>(config.option("printable_area"));
    if (opt != nullptr) {
        BoundingBox bed_box_2D = get_extents(Polygon::new_scale(opt->values));
        BoundingBoxf3 print_volume({ unscale<double>(bed_box_2D.min(0)), unscale<double>(bed_box_2D.min(1)), 0.0 }, { unscale<double>(bed_box_2D.max(0)), unscale<double>(bed_box_2D.max(1)), config.opt_float("printable_height") });
        Vec3d print_volume_size = print_volume.size();
        double sx = (box_size(0) != 0.0) ? print_volume_size(0) / box_size(0) : 0.0;
        double sy = (box_size(1) != 0.0) ? print_volume_size(1) / box_size(1) : 0.0;
        double sz = (box_size(2) != 0.0) ? print_volume_size(2) / box_size(2) : 0.0;
        if (sx != 0.0 && sy != 0.0 && sz != 0.0)
        {
            double s = std::min(sx, std::min(sy, sz));
            if (s != 1.0) {
                wxGetApp().plater()->take_snapshot("Scale To Fit");

                TransformationType type;
                type.set_world();
                type.set_relative();
                type.set_joint();

                // apply scale
                start_dragging();
                scale(s * Vec3d::Ones(), type);
                wxGetApp().plater()->canvas3D()->do_scale(""); // avoid storing another snapshot

                // center selection on print bed
                start_dragging();
                translate(print_volume.center() - get_bounding_box().center());
                wxGetApp().plater()->canvas3D()->do_move(""); // avoid storing another snapshot

                // BBS
                //wxGetApp().obj_manipul()->set_dirty();
            }
        }
    }
}
#endif // ENABLE_ENHANCED_PRINT_VOLUME_FIT

void Selection::scale_and_translate(const Vec3d &scale, const Vec3d &world_translation, TransformationType transformation_type)
{
    if (!m_valid) return;

    Vec3d relative_scale = scale;
    if (transformation_type.absolute()) {
        // converts to relative scale
        if (m_mode == Instance) {
            if (is_single_full_instance()) {
                BoundingBoxf3 current_box = get_bounding_box_in_current_reference_system().first;
                BoundingBoxf3 original_box;
                if (transformation_type.world())
                    original_box = get_full_unscaled_instance_bounding_box();
                else
                    original_box = get_full_unscaled_instance_local_bounding_box();

                relative_scale = original_box.size().cwiseProduct(scale).cwiseQuotient(current_box.size());
            }
        }
        transformation_type.set_relative();
    }

    for (unsigned int i : m_list) {
        GLVolume &                      v           = *(*m_volumes)[i];
        const VolumeCache &             volume_data = m_cache.volumes_data[i];
        const Geometry::Transformation &inst_trafo  = volume_data.get_instance_transform();
        auto                            old_rotate  = inst_trafo.get_rotation();
        if (m_mode == Instance) {
            if (transformation_type.instance()) {
                const Vec3d world_inst_pivot = m_cache.dragging_center - inst_trafo.get_offset();
                const Vec3d local_inst_pivot = inst_trafo.get_matrix_no_offset().inverse() * world_inst_pivot;
                Matrix3d    inst_rotation, inst_scale;
                inst_trafo.get_matrix().computeRotationScaling(&inst_rotation, &inst_scale);
                const Transform3d offset_trafo = Geometry::translation_transform(inst_trafo.get_offset() + world_translation);
                const Transform3d scale_trafo  = Transform3d(inst_scale) * Geometry::scale_transform(relative_scale);
                v.set_instance_transformation(Geometry::translation_transform(world_inst_pivot) * offset_trafo * Transform3d(inst_rotation) * scale_trafo *
                                              Geometry::translation_transform(-local_inst_pivot));
            } else
                transform_instance_relative(v, volume_data, transformation_type, Geometry::translation_transform(world_translation) * Geometry::scale_transform(relative_scale),
                                            m_cache.dragging_center);
        } else {
            if (!is_single_volume_or_modifier()) {
                assert(transformation_type.world());
                transform_volume_relative(v, volume_data, transformation_type, Geometry::translation_transform(world_translation) * Geometry::scale_transform(scale),
                                          m_cache.dragging_center);
            } else {
                transformation_type.set_independent();
                Vec3d translation;
                if (transformation_type.local()) {
                    translation = volume_data.get_volume_transform().get_matrix_no_offset().inverse() * inst_trafo.get_matrix_no_offset().inverse() * world_translation;
                }
                else if (transformation_type.instance())
                    translation = inst_trafo.get_matrix_no_offset().inverse() * world_translation;
                else
                    translation = world_translation;
                transform_volume_relative(v, volume_data, transformation_type, Geometry::translation_transform(translation) * Geometry::scale_transform(scale),
                                          m_cache.dragging_center);
            }
        }
    }

#if !DISABLE_INSTANCES_SYNCH
    if (m_mode == Instance)
        // even if there is no rotation, we pass SyncRotationType::GENERAL to force
        // synchronize_unselected_instances() to apply the scale to the other instances
        synchronize_unselected_instances(SyncRotationType::GENERAL);
    else if (m_mode == Volume)
        synchronize_unselected_volumes();
#endif // !DISABLE_INSTANCES_SYNCH

    ensure_on_bed();
    set_bounding_boxes_dirty();
    if (wxGetApp().plater()->canvas3D()->get_canvas_type() != GLCanvas3D::ECanvasType::CanvasAssembleView) {
        wxGetApp().plater()->canvas3D()->requires_check_outside_state();
    }
}

void Selection::mirror(Axis axis, TransformationType transformation_type)
{
    const Vec3d mirror((axis == X) ? -1.0 : 1.0, (axis == Y) ? -1.0 : 1.0, (axis == Z) ? -1.0 : 1.0);
    scale_and_translate(mirror, Vec3d::Zero(), transformation_type);

}

void Selection::translate(unsigned int object_idx, const Vec3d& displacement)
{
    if (!m_valid)
        return;

    //BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": obj %1%") % object_idx;
    //BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": %1%, displacement {%2%, %3%, %4%}") % __LINE__ % displacement(X) % displacement(Y) % displacement(Z);
    for (unsigned int i : m_list) {
        GLVolume& v = *(*m_volumes)[i];
        if (v.object_idx() == (int)object_idx)
            v.set_instance_offset(v.get_instance_offset() + displacement);
    }

    std::set<unsigned int> done;  // prevent processing volumes twice
    done.insert(m_list.begin(), m_list.end());

    for (unsigned int i : m_list) {
        if (done.size() == m_volumes->size())
            break;

        int object_idx = (*m_volumes)[i]->object_idx();
        if (object_idx >= 1000)
            continue;

        // Process unselected volumes of the object.
        for (unsigned int j = 0; j < (unsigned int)m_volumes->size(); ++j) {
            if (done.size() == m_volumes->size())
                break;

            if (done.find(j) != done.end())
                continue;

            GLVolume& v = *(*m_volumes)[j];
            if (v.object_idx() != object_idx)
                continue;

            v.set_instance_offset(v.get_instance_offset() + displacement);
            done.insert(j);
        }
    }

    this->set_bounding_boxes_dirty();
}

void Selection::translate(unsigned int object_idx, unsigned int instance_idx, const Vec3d& displacement)
{
    if (!m_valid)
        return;

    //BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": obj %1%, instance %2%") % object_idx % instance_idx;
    //BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": %1%, displacement {%2%, %3%, %4%}") % __LINE__ % displacement(X) % displacement(Y) % displacement(Z);
    for (unsigned int i : m_list) {
        GLVolume& v = *(*m_volumes)[i];
        if (v.object_idx() == (int)object_idx && v.instance_idx() == (int)instance_idx)
            v.set_instance_offset(v.get_instance_offset() + displacement);
    }

    std::set<unsigned int> done;  // prevent processing volumes twice
    done.insert(m_list.begin(), m_list.end());

    for (unsigned int i : m_list) {
        if (done.size() == m_volumes->size())
            break;

        int object_idx = (*m_volumes)[i]->object_idx();
        if (object_idx >= 1000)
            continue;

        // Process unselected volumes of the object.
        for (unsigned int j = 0; j < (unsigned int)m_volumes->size(); ++j) {
            if (done.size() == m_volumes->size())
                break;

            if (done.find(j) != done.end())
                continue;

            GLVolume& v = *(*m_volumes)[j];
            if (v.object_idx() != object_idx || v.instance_idx() != (int)instance_idx)
                continue;

            v.set_instance_offset(v.get_instance_offset() + displacement);
            done.insert(j);
        }
    }

    this->set_bounding_boxes_dirty();
}

void Selection::translate(unsigned int object_idx, unsigned int instance_idx, unsigned int volume_idx, const Vec3d &displacement) {
    if (!m_valid) return;

    for (unsigned int i : m_list) {
        GLVolume &v = *(*m_volumes)[i];
        if (v.object_idx() == (int) object_idx && v.instance_idx() == (int) instance_idx && v.volume_idx() == (int) volume_idx)
            v.set_volume_offset(v.get_volume_offset() + displacement);
    }

    this->set_bounding_boxes_dirty();
}

void Selection::rotate(unsigned int object_idx, unsigned int instance_idx, const Transform3d &overwrite_tran)
{
    if (!m_valid) return;

    for (unsigned int i : m_list) {
        GLVolume &v = *(*m_volumes)[i];
        if (v.object_idx() == (int) object_idx && v.instance_idx() == (int) instance_idx) {
            v.set_instance_transformation(overwrite_tran);
        }
    }

    std::set<unsigned int> done; // prevent processing volumes twice
    done.insert(m_list.begin(), m_list.end());
    for (unsigned int i : m_list) {
        if (done.size() == m_volumes->size()) break;

        int object_idx = (*m_volumes)[i]->object_idx();
        if (object_idx >= 1000) continue;

        // Process unselected volumes of the object.
        for (unsigned int j = 0; j < (unsigned int) m_volumes->size(); ++j) {
            if (done.size() == m_volumes->size()) break;

            if (done.find(j) != done.end()) continue;

            GLVolume &v = *(*m_volumes)[j];
            if (v.object_idx() != object_idx || v.instance_idx() != (int) instance_idx)
                continue;

            v.set_instance_transformation(overwrite_tran);
            done.insert(j);
        }
    }
    this->set_bounding_boxes_dirty();
}
void Selection::rotate(unsigned int object_idx, unsigned int instance_idx, unsigned int volume_idx, const Transform3d &overwrite_tran)
{
    if (!m_valid) return;

    for (unsigned int i : m_list) {
        GLVolume &v = *(*m_volumes)[i];
        if (v.object_idx() == (int) object_idx && v.instance_idx() == (int) instance_idx && v.volume_idx() == (int) volume_idx) {
            v.set_volume_transformation(overwrite_tran);
        }
    }
    this->set_bounding_boxes_dirty();
}

//BBS: add partplate related logic
void Selection::notify_instance_update(int object_idx, int instance_idx)
{
    //BBS: notify instance updates to part plater list
    PartPlateList& plate_list = wxGetApp().plater()->get_partplate_list();

    if (object_idx == -1)
    {
        std::set<std::pair<int, int>> notify_set;
        auto list = m_list;
        for (unsigned int i : list)
        {
            int obj_index = (*m_volumes)[i]->object_idx();
            //-1 means all the instance in this object
            if (instance_idx == -1)
            {
                ModelObject* object = m_model->objects[obj_index];

                for (int instance_index = 0; instance_index < object->instances.size(); instance_index++)
                {
                    std::pair<int, int> notify_index(obj_index, instance_index);
                    if (notify_set.find(notify_index) == notify_set.end()) {
                        plate_list.notify_instance_update(obj_index, instance_index);
                        notify_set.insert(notify_index);
                    }
                }
            }
            else {
                std::pair<int, int> notify_index(obj_index, instance_idx);
                if (notify_set.find(notify_index) == notify_set.end()) {
                    plate_list.notify_instance_update(obj_index, instance_idx);
                    notify_set.insert(notify_index);
                }
            }
        }
    }
    else
    {
        if (instance_idx == -1)
        {
            ModelObject* object = m_model->objects[object_idx];

            for (int index = 0; index < object->instances.size(); index++)
            {
                plate_list.notify_instance_update(object_idx, index);
            }
        }
        else
            plate_list.notify_instance_update(object_idx, instance_idx);
    }
}

void Selection::erase()
{
    if (!m_valid)
        return;

    if (is_single_full_object())
        wxGetApp().obj_list()->delete_from_model_and_list(ItemType::itObject, get_object_idx(), 0);
    else if (is_multiple_full_object()) {
        std::vector<ItemForDelete> items;
        items.reserve(m_cache.content.size());
        for (ObjectIdxsToInstanceIdxsMap::iterator it = m_cache.content.begin(); it != m_cache.content.end(); ++it) {
            items.emplace_back(ItemType::itObject, it->first, 0);
        }
        wxGetApp().obj_list()->delete_from_model_and_list(items);
    }
    else if (is_multiple_full_instance()) {
        std::set<std::pair<int, int>> instances_idxs;
        for (ObjectIdxsToInstanceIdxsMap::iterator obj_it = m_cache.content.begin(); obj_it != m_cache.content.end(); ++obj_it) {
            for (InstanceIdxsList::reverse_iterator inst_it = obj_it->second.rbegin(); inst_it != obj_it->second.rend(); ++inst_it) {
                instances_idxs.insert(std::make_pair(obj_it->first, *inst_it));
            }
        }

        std::vector<ItemForDelete> items;
        items.reserve(instances_idxs.size());
        for (const std::pair<int, int>& i : instances_idxs) {
            items.emplace_back(ItemType::itInstance, i.first, i.second);
        }
        wxGetApp().obj_list()->delete_from_model_and_list(items);
    }
    else if (is_single_full_instance())
        wxGetApp().obj_list()->delete_from_model_and_list(ItemType::itInstance, get_object_idx(), get_instance_idx());
    else if (is_mixed()) {
        std::set<ItemForDelete> items_set;
        std::map<int, int> volumes_in_obj;

        for (auto i : m_list) {
            const auto gl_vol = (*m_volumes)[i];
            const auto glv_obj_idx = gl_vol->object_idx();
            const auto model_object = m_model->objects[glv_obj_idx];

            if (model_object->instances.size() == 1) {
                if (model_object->volumes.size() == 1)
                    items_set.insert(ItemForDelete(ItemType::itObject, glv_obj_idx, -1));
                else {
                    items_set.insert(ItemForDelete(ItemType::itVolume, glv_obj_idx, gl_vol->volume_idx()));
                    int idx = (volumes_in_obj.find(glv_obj_idx) == volumes_in_obj.end()) ? 0 : volumes_in_obj.at(glv_obj_idx);
                    volumes_in_obj[glv_obj_idx] = ++idx;
                }
                continue;
            }

            const auto glv_ins_idx = gl_vol->instance_idx();

            for (auto obj_ins : m_cache.content) {
                if (obj_ins.first == glv_obj_idx) {
                    if (obj_ins.second.find(glv_ins_idx) != obj_ins.second.end()) {
                        if (obj_ins.second.size() == model_object->instances.size())
                            items_set.insert(ItemForDelete(ItemType::itObject, glv_obj_idx, -1));
                        else
                            items_set.insert(ItemForDelete(ItemType::itInstance, glv_obj_idx, glv_ins_idx));

                        break;
                    }
                }
            }
        }

        std::vector<ItemForDelete> items;
        items.reserve(items_set.size());
        for (const ItemForDelete& i : items_set) {
            if (i.type == ItemType::itVolume) {
                const int vol_in_obj_cnt = volumes_in_obj.find(i.obj_idx) == volumes_in_obj.end() ? 0 : volumes_in_obj.at(i.obj_idx);
                if (vol_in_obj_cnt == (int)m_model->objects[i.obj_idx]->volumes.size()) {
                    if (i.sub_obj_idx == vol_in_obj_cnt - 1)
                        items.emplace_back(ItemType::itObject, i.obj_idx, 0);
                    continue;
                }
            }
            items.emplace_back(i.type, i.obj_idx, i.sub_obj_idx);
        }

        wxGetApp().obj_list()->delete_from_model_and_list(items);
    }
    else {
        std::set<std::pair<int, int>> volumes_idxs;
        for (unsigned int i : m_list) {
            const GLVolume* v = (*m_volumes)[i];
            // Only remove volumes associated with ModelVolumes from the object list.
            // Temporary meshes (SLA supports or pads) are not managed by the object list.
            if (v->volume_idx() >= 0)
                volumes_idxs.insert(std::make_pair(v->object_idx(), v->volume_idx()));
        }

        std::vector<ItemForDelete> items;
        items.reserve(volumes_idxs.size());
        for (const std::pair<int, int>& v : volumes_idxs) {
            items.emplace_back(ItemType::itVolume, v.first, v.second);
        }

        wxGetApp().obj_list()->delete_from_model_and_list(items);
        ensure_not_below_bed();
    }
}

void Selection::render(float scale_factor) const
{
    if (!m_valid || is_empty())
        return;

    *const_cast<float*>(&m_scale_factor) = scale_factor;

    // render cumulative bounding box of selected volumes
    render_selected_volumes();
    render_synchronized_volumes();
}

#if ENABLE_RENDER_SELECTION_CENTER
void Selection::render_center(bool gizmo_is_dragging) const
{
    if (!m_valid || is_empty())
        return;

    const auto& shader = wxGetApp().get_shader("flat");
    if (!shader)
        return;

    wxGetApp().bind_shader(shader);

    const Vec3d center = gizmo_is_dragging ? m_cache.dragging_center : get_bounding_box().center();

    glsafe(::glDisable(GL_DEPTH_TEST));

    const Camera& camera = wxGetApp().plater()->get_camera();
    Transform3d view_model_matrix = camera.get_view_matrix() * Geometry::assemble_transform(center);

    shader->set_uniform("view_model_matrix", view_model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    m_vbo_sphere.set_color(ColorRGBA::WHITE());
    m_vbo_sphere.render_geometry();

    wxGetApp().unbind_shader();
}
#endif // ENABLE_RENDER_SELECTION_CENTER

//BBS: GUI refactor, add uniform scale from gizmo
void Selection::render_sidebar_hints(const std::string& sidebar_field, bool uniform_scale) const
//void Selection::render_sidebar_hints(const std::string& sidebar_field) const
{
    if (sidebar_field.empty())
        return;

    const auto& shader = wxGetApp().get_shader(boost::starts_with(sidebar_field, "layer") ? "flat" : "gouraud_light");
    if (shader == nullptr)
        return;

    wxGetApp().bind_shader(shader);

    if (!boost::starts_with(sidebar_field, "layer")) {
        glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    }
    glsafe(::glEnable(GL_DEPTH_TEST));

    Transform3d base_matrix = Geometry::assemble_transform(get_bounding_box().center());
    Transform3d orient_matrix = Transform3d::Identity();
    if (!boost::starts_with(sidebar_field, "layer")) {
        Vec3d center = get_bounding_box().center();
        const auto &[box, box_trafo] = get_bounding_box_in_current_reference_system();
        // BBS
        if (is_single_full_instance() && !wxGetApp().obj_manipul()->is_world_coordinates()) {
            orient_matrix = (*m_volumes)[*m_list.begin()]->get_instance_transformation().get_rotation_matrix();
        } else if (is_single_volume_or_modifier()) {
            if (!wxGetApp().obj_manipul()->is_world_coordinates()) {
                if (wxGetApp().obj_manipul()->is_local_coordinates()) {
                    orient_matrix               = get_bounding_box_in_current_reference_system().second;
                    orient_matrix.translation() = Vec3d::Zero();
                } else {
                    orient_matrix = (*m_volumes)[*m_list.begin()]->get_instance_transformation().get_rotation_matrix();
                    center       = box_trafo.translation();
                }
            }
            base_matrix = Geometry::assemble_transform({ center(0), center(1), center(2) });
        } else {
            if (requires_local_axes()) {
                orient_matrix = (*m_volumes)[*m_list.begin()]->get_instance_transformation().get_rotation_matrix();
            }
        }
    }

    if (boost::starts_with(sidebar_field, "position"))
        render_sidebar_position_hints(sidebar_field, *shader, base_matrix * orient_matrix);
    else if (boost::starts_with(sidebar_field, "rotation") || boost::starts_with(sidebar_field, "absolute_rotation"))
        render_sidebar_rotation_hints(sidebar_field, *shader, base_matrix * orient_matrix);
    else if (boost::starts_with(sidebar_field, "scale") || boost::starts_with(sidebar_field, "size"))
        //BBS: GUI refactor: add uniform_scale from gizmo
        render_sidebar_scale_hints(sidebar_field, uniform_scale, *shader, base_matrix * orient_matrix);
    else if (boost::starts_with(sidebar_field, "layer"))
        render_sidebar_layers_hints(*shader, sidebar_field);

    wxGetApp().unbind_shader();
}

bool Selection::requires_local_axes() const
{
    return (m_mode == Volume) && is_from_single_instance();
}

void Selection::cut_to_clipboard()
{
    copy_to_clipboard();
    erase();
}

void Selection::copy_to_clipboard()
{
    if (!m_valid)
        return;

    m_clipboard.reset();

    // sort as the object list order
    std::vector<unsigned int> selected_list;
    selected_list.assign(m_list.begin(), m_list.end());
    std::sort(selected_list.begin(), selected_list.end(), [this](unsigned int left, unsigned int right) {
        return (*m_volumes)[left]->volume_idx() < (*m_volumes)[right]->volume_idx();
    });

    for (const ObjectIdxsToInstanceIdxsMap::value_type& object : m_cache.content)
    {
        ModelObject* src_object = m_model->objects[object.first];
        ModelObject* dst_object = m_clipboard.add_object();
        dst_object->name                 = src_object->name;
        dst_object->input_file           = src_object->input_file;
		dst_object->config.assign_config(src_object->config);
        dst_object->sla_support_points   = src_object->sla_support_points;
        dst_object->sla_points_status    = src_object->sla_points_status;
        dst_object->sla_drain_holes      = src_object->sla_drain_holes;
        dst_object->brim_points          = src_object->brim_points;
        dst_object->layer_config_ranges  = src_object->layer_config_ranges;     // #ys_FIXME_experiment
        dst_object->layer_height_profile.assign(src_object->layer_height_profile);
        dst_object->origin_translation   = src_object->origin_translation;

        for (int i : object.second)
        {
            dst_object->add_instance(*src_object->instances[i]);
        }

        for (unsigned int i : selected_list)
        {
            // Copy the ModelVolumes only for the selected GLVolumes of the 1st selected instance.
            const GLVolume* volume = (*m_volumes)[i];
            if ((volume->object_idx() == object.first) && (volume->instance_idx() == *object.second.begin()))
            {
                int volume_idx = volume->volume_idx();
                if ((0 <= volume_idx) && (volume_idx < (int)src_object->volumes.size()))
                {
                    ModelVolume* src_volume = src_object->volumes[volume_idx];
                    ModelVolume* dst_volume = dst_object->add_volume(*src_volume);
                    dst_volume->set_new_unique_id();
                } else {
                    assert(false);
                }
            }
        }
    }

    m_clipboard.set_mode(m_mode);
}

void Selection::paste_from_clipboard()
{
    if (!m_valid || m_clipboard.is_empty())
        return;

    switch (m_clipboard.get_mode())
    {
    case Volume:
    {
        if (is_from_single_instance())
            paste_volumes_from_clipboard();

        break;
    }
    case Instance:
    {
        if (m_mode == Instance)
            paste_objects_from_clipboard();

        break;
    }
    }
}

//BBS get export mesh for exporting stl
std::set<std::pair<int, int>> Selection::get_selected_object_instances()
{
    std::set<std::pair<int, int>> instances_idxs;
    // BBS only support multi full object now
    if (!is_multiple_full_object())
        return instances_idxs;

    for (ObjectIdxsToInstanceIdxsMap::iterator obj_it = m_cache.content.begin(); obj_it != m_cache.content.end(); ++obj_it)
    {
        for (InstanceIdxsList::reverse_iterator inst_it = obj_it->second.rbegin(); inst_it != obj_it->second.rend(); ++inst_it)
        {
            instances_idxs.insert(std::make_pair(obj_it->first, *inst_it));
        }
    }

    return instances_idxs;
}

void Selection::fill_color(int extruder_id)
{
    wxGetApp().obj_list()->set_extruder_for_selected_items(extruder_id);
}

std::vector<unsigned int> Selection::get_volume_idxs_from_object(unsigned int object_idx) const
{
    std::vector<unsigned int> idxs;

    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i)
    {
        if ((*m_volumes)[i]->object_idx() == (int)object_idx)
            idxs.push_back(i);
    }

    return idxs;
}

std::vector<unsigned int> Selection::get_volume_idxs_from_instance(unsigned int object_idx, unsigned int instance_idx) const
{
    std::vector<unsigned int> idxs;

    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i)
    {
        const GLVolume* v = (*m_volumes)[i];
        if ((v->object_idx() == (int)object_idx) && (v->instance_idx() == (int)instance_idx))
            idxs.push_back(i);
    }

    return idxs;
}

std::vector<unsigned int> Selection::get_volume_idxs_from_volume(unsigned int object_idx, unsigned int instance_idx, unsigned int volume_idx) const
{
    std::vector<unsigned int> idxs;

    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i)
    {
        const GLVolume* v = (*m_volumes)[i];
        if ((v->object_idx() == (int)object_idx) && (v->volume_idx() == (int)volume_idx))
        {
            if (((int)instance_idx != -1) && (v->instance_idx() == (int)instance_idx))
                idxs.push_back(i);
        }
    }

    return idxs;
}

std::vector<unsigned int> Selection::get_missing_volume_idxs_from(const std::vector<unsigned int>& volume_idxs) const
{
    std::vector<unsigned int> idxs;

    for (unsigned int i : m_list)
    {
        std::vector<unsigned int>::const_iterator it = std::find(volume_idxs.begin(), volume_idxs.end(), i);
        if (it == volume_idxs.end())
            idxs.push_back(i);
    }

    return idxs;
}

std::vector<unsigned int> Selection::get_unselected_volume_idxs_from(const std::vector<unsigned int>& volume_idxs) const
{
    std::vector<unsigned int> idxs;

    for (unsigned int i : volume_idxs)
    {
        if (m_list.find(i) == m_list.end())
            idxs.push_back(i);
    }

    return idxs;
}

void Selection::update_valid()
{
    m_valid = (m_volumes != nullptr) && (m_model != nullptr);
}

void Selection::update_type()
{
    m_cache.content.clear();
    m_type = Mixed;

    for (unsigned int i : m_list)
    {
        const GLVolume* volume = (*m_volumes)[i];
        int obj_idx = volume->object_idx();
        int inst_idx = volume->instance_idx();
        ObjectIdxsToInstanceIdxsMap::iterator obj_it = m_cache.content.find(obj_idx);
        if (obj_it == m_cache.content.end())
            obj_it = m_cache.content.insert(ObjectIdxsToInstanceIdxsMap::value_type(obj_idx, InstanceIdxsList())).first;

        obj_it->second.insert(inst_idx);
    }

    bool requires_disable = false;

    if (!m_valid)
        m_type = Invalid;
    else
    {
        if (m_list.empty())
            m_type = Empty;
        else if (m_list.size() == 1)
        {
            const GLVolume* first = (*m_volumes)[*m_list.begin()];
            if (first->is_wipe_tower)
                m_type = WipeTower;
            else if (first->is_modifier)
            {
                m_type = SingleModifier;
                requires_disable = true;
            }
            else
            {
                const ModelObject* model_object = m_model->objects[first->object_idx()];
                unsigned int volumes_count = (unsigned int)model_object->volumes.size();
                unsigned int instances_count = (unsigned int)model_object->instances.size();
                if (volumes_count * instances_count == 1)
                {
                    m_type = SingleFullObject;
                    // ensures the correct mode is selected
                    m_mode = Instance;
                }
                else if (volumes_count == 1) // instances_count > 1
                {
                    m_type = SingleFullInstance;
                    // ensures the correct mode is selected
                    m_mode = Instance;
                }
                else
                {
                    m_type = SingleVolume;
                    requires_disable = true;
                }
            }
        }
        else
        {
            unsigned int sla_volumes_count = 0;
            // Note: sla_volumes_count is a count of the selected sla_volumes per object instead of per instance, like a model_volumes_count is
            for (unsigned int i : m_list) {
                if ((*m_volumes)[i]->volume_idx() < 0)
                    ++sla_volumes_count;
            }

            if (m_cache.content.size() == 1) // single object
            {
                const ModelObject* model_object = m_model->objects[m_cache.content.begin()->first];
                unsigned int model_volumes_count = (unsigned int)model_object->volumes.size();

                unsigned int instances_count = (unsigned int)model_object->instances.size();
                unsigned int selected_instances_count = (unsigned int)m_cache.content.begin()->second.size();
                if (model_volumes_count * instances_count + sla_volumes_count == (unsigned int)m_list.size())
                {
                    m_type = SingleFullObject;
                    // ensures the correct mode is selected
                    m_mode = Instance;
                }
                else if (selected_instances_count == 1)
                {
                    if (model_volumes_count + sla_volumes_count == (unsigned int)m_list.size())
                    {
                        m_type = SingleFullInstance;
                        // ensures the correct mode is selected
                        m_mode = Instance;
                    }
                    else
                    {
                        unsigned int modifiers_count = 0;
                        for (unsigned int i : m_list)
                        {
                            if ((*m_volumes)[i]->is_modifier)
                                ++modifiers_count;
                        }

                        if (modifiers_count == 0)
                            m_type = MultipleVolume;
                        else if (modifiers_count == (unsigned int)m_list.size())
                            m_type = MultipleModifier;

                        requires_disable = true;
                    }
                }
                else if ((selected_instances_count > 1) && (selected_instances_count * model_volumes_count + sla_volumes_count == (unsigned int)m_list.size()))
                {
                    m_type = MultipleFullInstance;
                    // ensures the correct mode is selected
                    m_mode = Instance;
                }
            }
            else
            {
                unsigned int sels_cntr = 0;
                for (ObjectIdxsToInstanceIdxsMap::iterator it = m_cache.content.begin(); it != m_cache.content.end(); ++it)
                {
                    bool               is_wipe_tower   = it->first >= 1000;
                    int                actual_obj_id   = is_wipe_tower ? it->first - 1000 : it->first;
                    const ModelObject *model_object    = m_model->objects[actual_obj_id];
                    unsigned int volumes_count = (unsigned int)model_object->volumes.size();
                    unsigned int instances_count = (unsigned int)model_object->instances.size();
                    sels_cntr += volumes_count * instances_count;
                }
                if (sels_cntr + sla_volumes_count == (unsigned int)m_list.size())
                {
                    m_type = MultipleFullObject;
                    // ensures the correct mode is selected
                    m_mode = Instance;
                }
            }
        }
    }

    //BBS: remove the disable logic here
    /*int object_idx = get_object_idx();
    int instance_idx = get_instance_idx();
    for (GLVolume* v : *m_volumes)
    {
        v->disabled = requires_disable ? (v->object_idx() != object_idx) || (v->instance_idx() != instance_idx) : false;
    }*/

#if ENABLE_SELECTION_DEBUG_OUTPUT
    std::cout << "Selection: ";
    std::cout << "mode: ";
    switch (m_mode)
    {
    case Volume:
    {
        std::cout << "Volume";
        break;
    }
    case Instance:
    {
        std::cout << "Instance";
        break;
    }
    }

    std::cout << " - type: ";

    switch (m_type)
    {
    case Invalid:
    {
        std::cout << "Invalid" << std::endl;
        break;
    }
    case Empty:
    {
        std::cout << "Empty" << std::endl;
        break;
    }
    case WipeTower:
    {
        std::cout << "WipeTower" << std::endl;
        break;
    }
    case SingleModifier:
    {
        std::cout << "SingleModifier" << std::endl;
        break;
    }
    case MultipleModifier:
    {
        std::cout << "MultipleModifier" << std::endl;
        break;
    }
    case SingleVolume:
    {
        std::cout << "SingleVolume" << std::endl;
        break;
    }
    case MultipleVolume:
    {
        std::cout << "MultipleVolume" << std::endl;
        break;
    }
    case SingleFullObject:
    {
        std::cout << "SingleFullObject" << std::endl;
        break;
    }
    case MultipleFullObject:
    {
        std::cout << "MultipleFullObject" << std::endl;
        break;
    }
    case SingleFullInstance:
    {
        std::cout << "SingleFullInstance" << std::endl;
        break;
    }
    case MultipleFullInstance:
    {
        std::cout << "MultipleFullInstance" << std::endl;
        break;
    }
    case Mixed:
    {
        std::cout << "Mixed" << std::endl;
        break;
    }
    }
#endif // ENABLE_SELECTION_DEBUG_OUTPUT
}

void Selection::set_caches()
{
    m_cache.volumes_data.clear();
    m_cache.sinking_volumes.clear();
    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i) {
        const GLVolume& v = *(*m_volumes)[i];
        m_cache.volumes_data.emplace(i, VolumeCache(v.get_volume_transformation(), v.get_instance_transformation()));
        if (v.is_sinking())
            m_cache.sinking_volumes.push_back(i);
    }
    m_cache.dragging_center = get_bounding_box().center();
    m_cache.rotation_pivot  = get_bounding_sphere().first;
}

void Selection::do_add_volume(unsigned int volume_idx)
{
    m_list.insert(volume_idx);
    GLVolume* v = (*m_volumes)[volume_idx];
    v->selected = true;
    if (v->hover == GLVolume::HS_Select || v->hover == GLVolume::HS_Deselect)
        v->hover = GLVolume::HS_Hover;
}

void Selection::do_add_volumes(const std::vector<unsigned int>& volume_idxs)
{
    for (unsigned int i : volume_idxs)
    {
        if (i < (unsigned int)m_volumes->size())
            do_add_volume(i);
    }
}

void Selection::do_remove_volume(unsigned int volume_idx)
{
    IndicesList::iterator v_it = m_list.find(volume_idx);
    if (v_it == m_list.end())
        return;

    m_list.erase(v_it);

    (*m_volumes)[volume_idx]->selected = false;
}

void Selection::do_remove_instance(unsigned int object_idx, unsigned int instance_idx)
{
    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i) {
        GLVolume* v = (*m_volumes)[i];
        if (v->object_idx() == (int)object_idx && v->instance_idx() == (int)instance_idx)
            do_remove_volume(i);
    }
}

void Selection::do_remove_object(unsigned int object_idx)
{
    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i) {
        GLVolume* v = (*m_volumes)[i];
        if (v->object_idx() == (int)object_idx)
            do_remove_volume(i);
    }
}

void Selection::render_selected_volumes() const
{
    float color[3] = { 1.0f, 1.0f, 1.0f };
    render_bounding_box(get_bounding_box(), color);
}

void Selection::render_synchronized_volumes() const
{
    if (m_mode == Instance)
        return;

    float color[3] = { 1.0f, 1.0f, 0.0f };

    for (unsigned int i : m_list) {
        const GLVolume* volume = (*m_volumes)[i];
        int object_idx = volume->object_idx();
        int volume_idx = volume->volume_idx();
        for (unsigned int j = 0; j < (unsigned int)m_volumes->size(); ++j) {
            if (i == j)
                continue;

            const GLVolume* v = (*m_volumes)[j];
            if (v->object_idx() != object_idx || v->volume_idx() != volume_idx)
                continue;

            render_bounding_box(v->transformed_convex_hull_bounding_box(), color);
        }
    }
}

void Selection::render_bounding_box(const BoundingBoxf3& box, float* color) const
{
    if (color == nullptr)
        return;

    const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
    const auto& p_flat_shader = wxGetApp().get_shader("flat");
    if (!p_flat_shader)
        return;

    init_bounding_box_model();

    Vec3f b_min = box.min.cast<float>();
    Vec3f b_max = box.max.cast<float>();
    Vec3f size = box.size().cast<float>();
    const auto& center = box.center();

    Transform3d model_matrix{ Transform3d::Identity() };
    model_matrix.data()[3 * 4 + 0] = center(0);
    model_matrix.data()[3 * 4 + 1] = center(1);
    model_matrix.data()[3 * 4 + 2] = center(2);
    model_matrix.data()[0 * 4 + 0] = size(0);
    model_matrix.data()[1 * 4 + 1] = size(1);
    model_matrix.data()[2 * 4 + 2] = size(2);

    glsafe(::glEnable(GL_DEPTH_TEST));

    p_ogl_manager->set_line_width(2.0f * m_scale_factor);

    wxGetApp().bind_shader(p_flat_shader);

    const Camera& camera = wxGetApp().plater()->get_camera();
    const auto& view_matrix = camera.get_view_matrix();
    const auto& proj_matrix = camera.get_projection_matrix();
    p_flat_shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
    p_flat_shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    m_bounding_box_model.set_color({ color[0], color[1], color[2], 1.0f});
    m_bounding_box_model.render_geometry();

    wxGetApp().unbind_shader();
}

static std::array<float, 4> get_color(Axis axis)
{
    return { GLGizmoBase::AXES_COLOR[axis][0],
            GLGizmoBase::AXES_COLOR[axis][1],
            GLGizmoBase::AXES_COLOR[axis][2],
            GLGizmoBase::AXES_COLOR[axis][3] };
};

Transform3d get_screen_scalling_matrix()
{
    const Camera& camera = wxGetApp().plater()->get_camera();

    Transform3d screen_scalling_matrix{ Transform3d::Identity() };

    const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
    if (p_ogl_manager) {
        if (p_ogl_manager->is_gizmo_keep_screen_size_enabled()) {
            const auto& t_zoom = camera.get_zoom();
            screen_scalling_matrix.data()[0 * 4 + 0] = 5.0f / t_zoom;
            screen_scalling_matrix.data()[1 * 4 + 1] = 5.0f / t_zoom;
            screen_scalling_matrix.data()[2 * 4 + 2] = 5.0f / t_zoom;
        }
    }
    return screen_scalling_matrix;
}

void Selection::render_sidebar_position_hints(const std::string& sidebar_field, GLShaderProgram& shader, const Transform3d& model_matrix) const
{
    const Camera& camera = wxGetApp().plater()->get_camera();

    const Transform3d screen_scalling_matrix = get_screen_scalling_matrix();

    const Transform3d view_matrix = camera.get_view_matrix() * model_matrix * screen_scalling_matrix;
    shader.set_uniform("projection_matrix", camera.get_projection_matrix());

    if (boost::ends_with(sidebar_field, "x")) {
        const Transform3d view_model_matrix = view_matrix * Geometry::assemble_transform(Vec3d::Zero(), -0.5 * PI * Vec3d::UnitZ());
        shader.set_uniform("view_model_matrix", view_model_matrix);
        shader.set_uniform("normal_matrix", (Matrix3d)view_model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
        const_cast<GLModel*>(&m_arrow)->set_color(-1, get_color(X));
        m_arrow.render_geometry();
    }
    else if (boost::ends_with(sidebar_field, "y")) {
        shader.set_uniform("view_model_matrix", view_matrix);
        shader.set_uniform("normal_matrix", (Matrix3d)view_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
        const_cast<GLModel*>(&m_arrow)->set_color(-1, get_color(Y));
        m_arrow.render_geometry();
    }
    else if (boost::ends_with(sidebar_field, "z")) {
        const Transform3d view_model_matrix = view_matrix * Geometry::assemble_transform(Vec3d::Zero(), 0.5 * PI * Vec3d::UnitX());
        shader.set_uniform("view_model_matrix", view_model_matrix);
        shader.set_uniform("normal_matrix", (Matrix3d)view_model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
        const_cast<GLModel*>(&m_arrow)->set_color(-1, get_color(Z));
        m_arrow.render_geometry();
    }
}

void Selection::render_sidebar_rotation_hints(const std::string& sidebar_field, GLShaderProgram& shader, const Transform3d& model_matrix) const
{
    auto render_sidebar_rotation_hint = [this](GLShaderProgram& shader, const Transform3d& matrix) {

        const Camera& camera = wxGetApp().plater()->get_camera();

        const Transform3d screen_scalling_matrix = get_screen_scalling_matrix();

        Transform3d view_model_matrix = matrix * screen_scalling_matrix;
        shader.set_uniform("view_model_matrix", view_model_matrix);
        shader.set_uniform("normal_matrix", (Matrix3d)view_model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
        m_curved_arrow.render_geometry();

        view_model_matrix = matrix * Geometry::assemble_transform(Vec3d::Zero(), PI * Vec3d::UnitZ()) * screen_scalling_matrix;
        shader.set_uniform("view_model_matrix", view_model_matrix);
        shader.set_uniform("normal_matrix", (Matrix3d)view_model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
        m_curved_arrow.render_geometry();
    };

    const Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d view_matrix = camera.get_view_matrix() * model_matrix;
    shader.set_uniform("projection_matrix", camera.get_projection_matrix());

    if (boost::ends_with(sidebar_field, "x")) {
        const_cast<GLModel*>(&m_curved_arrow)->set_color(-1, get_color(X));
        render_sidebar_rotation_hint(shader, view_matrix * Geometry::assemble_transform(Vec3d::Zero(), 0.5 * PI * Vec3d::UnitY()));
    }
    else if (boost::ends_with(sidebar_field, "y")) {
        const_cast<GLModel*>(&m_curved_arrow)->set_color(-1, get_color(Y));
        render_sidebar_rotation_hint(shader, view_matrix * Geometry::assemble_transform(Vec3d::Zero(), -0.5 * PI * Vec3d::UnitX()));
    }
    else if (boost::ends_with(sidebar_field, "z")) {
        const_cast<GLModel*>(&m_curved_arrow)->set_color(-1, get_color(Z));
        render_sidebar_rotation_hint(shader, view_matrix);
    }
}

//BBS: GUI refactor: add gizmo uniform_scale
void Selection::render_sidebar_scale_hints(const std::string& sidebar_field, bool gizmo_uniform_scale, GLShaderProgram& shader, const Transform3d& model_matrix) const
{
    // BBS
    //bool uniform_scale = requires_uniform_scale() || wxGetApp().obj_manipul()->get_uniform_scaling();
    bool uniform_scale = requires_uniform_scale() || gizmo_uniform_scale;

    auto render_sidebar_scale_hint = [this, uniform_scale](Axis axis, GLShaderProgram& shader, const Transform3d& matrix) {
        const_cast<GLModel*>(&m_arrow)->set_color(-1, uniform_scale ? UNIFORM_SCALE_COLOR : get_color(axis));
        shader.set_uniform("emission_factor", 0.0f);

        Transform3d view_model_matrix = matrix * Geometry::assemble_transform(5.0 * Vec3d::UnitY());
        shader.set_uniform("view_model_matrix", view_model_matrix);
        shader.set_uniform("normal_matrix", (Matrix3d)view_model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
        m_arrow.render_geometry();

        view_model_matrix = matrix * Geometry::assemble_transform(-10.0 * Vec3d::UnitY(), PI * Vec3d::UnitZ());
        shader.set_uniform("view_model_matrix", view_model_matrix);
        shader.set_uniform("normal_matrix", (Matrix3d)view_model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
        m_arrow.render_geometry();
    };

    const Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d view_matrix = camera.get_view_matrix() * model_matrix;
    shader.set_uniform("projection_matrix", camera.get_projection_matrix());

    if (boost::ends_with(sidebar_field, "x") || uniform_scale) {
        render_sidebar_scale_hint(X, shader, view_matrix * Geometry::assemble_transform(Vec3d::Zero(), -0.5 * PI * Vec3d::UnitZ()));
    }

    if (boost::ends_with(sidebar_field, "y") || uniform_scale) {
        render_sidebar_scale_hint(Y, shader, view_matrix);
    }

    if (boost::ends_with(sidebar_field, "z") || uniform_scale) {
        render_sidebar_scale_hint(Z, shader, view_matrix * Geometry::assemble_transform(Vec3d::Zero(), 0.5 * PI * Vec3d::UnitX()));
    }
}

void Selection::render_sidebar_layers_hints(GLShaderProgram& shader, const std::string& sidebar_field) const
{
    static const double Margin = 10.0;
    if (wxGetApp().plater()->canvas3D()->get_canvas_type() != GLCanvas3D::ECanvasType::CanvasView3D) {
        return;
    }
    std::string field = sidebar_field;

    // extract max_z
    std::string::size_type pos = field.rfind("_");
    if (pos == std::string::npos)
        return;

    double max_z = string_to_double_decimal_point(field.substr(pos + 1));

    // extract min_z
    field = field.substr(0, pos);
    pos = field.rfind("_");
    if (pos == std::string::npos)
        return;

    const double min_z = string_to_double_decimal_point(field.substr(pos + 1));

    // extract type
    field = field.substr(0, pos);
    pos = field.rfind("_");
    if (pos == std::string::npos)
        return;

    const int type = std::stoi(field.substr(pos + 1));

    const BoundingBoxf3& box = get_bounding_box();

    const float min_x = box.min(0) - Margin;
    const float max_x = box.max(0) + Margin;
    const float min_y = box.min(1) - Margin;
    const float max_y = box.max(1) + Margin;

    // view dependend order of rendering to keep correct transparency
    bool camera_on_top = wxGetApp().plater()->get_camera().is_looking_downward();
    const float z1 = camera_on_top ? min_z : max_z;
    const float z2 = camera_on_top ? max_z : min_z;

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    if (!m_sidebar_layers_hints_model.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = { GLModel::PrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(4);
        init_data.reserve_indices(6);

        // vertices
        init_data.add_vertex(Vec3f(-0.5f, -0.5f, 0.0f));
        init_data.add_vertex(Vec3f(0.5f,  -0.5f, 0.0f));
        init_data.add_vertex(Vec3f(0.5f,   0.5f, 0.0f));
        init_data.add_vertex(Vec3f(-0.5f,  0.5f, 0.0f));

        // indices
        init_data.add_triangle(0, 1, 2);
        init_data.add_triangle(2, 3, 0);

        m_sidebar_layers_hints_model.init_from(std::move(init_data));
    }
    Transform3d model_matrix{ Transform3d::Identity() };
    model_matrix.data()[3 * 4 + 0] = (max_x + min_x) * 0.5f;
    model_matrix.data()[3 * 4 + 1] = (max_y + min_y) * 0.5f;
    model_matrix.data()[3 * 4 + 2] = z1;
    model_matrix.data()[0 * 4 + 0] = (max_x - min_x);
    model_matrix.data()[1 * 4 + 1] = (max_y - min_y);
    model_matrix.data()[2 * 4 + 2] = 1.0f;

    ColorRGBA color1;
    if ((camera_on_top && type == 1) || (!camera_on_top && type == 2)) {
        color1 = { 0.0f, 174.0f / 255.0f, 66.0f / 255.0f, 1.0f };
    }
    else {
        color1 = { 0.8f, 0.8f, 0.8f, 0.5 };
    }
    m_sidebar_layers_hints_model.set_color(color1);

    const Camera& camera = wxGetApp().plater()->get_camera();
    const auto& view_matrix = camera.get_view_matrix();
    const auto& proj_matrix = camera.get_projection_matrix();

    shader.set_uniform("projection_matrix", proj_matrix);

    shader.set_uniform("view_model_matrix", view_matrix * model_matrix);
    m_sidebar_layers_hints_model.render_geometry();

    model_matrix.data()[3 * 4 + 2] = z2;
    shader.set_uniform("view_model_matrix", view_matrix * model_matrix);
    m_sidebar_layers_hints_model.render_geometry();

    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));
}

void Selection::init_bounding_box_model() const
{
    if (m_bounding_box_model.is_initialized()) {
        return;
    }

    GLModel::Geometry geo;
    geo.format.type = GLModel::PrimitiveType::Lines;
    geo.format.vertex_layout = GLModel::Geometry::EVertexLayout::P3;

    const float size = 0.2f;
    Vec3f b_min{ -0.5f, -0.5f, -0.5f };
    Vec3f b_max{ 0.5f, 0.5f, 0.5f };
    geo.add_vertex(Vec3f{ b_min(0), b_min(1), b_min(2) });
    geo.add_vertex(Vec3f{ b_min(0) + size, b_min(1), b_min(2) });
    geo.add_vertex(Vec3f{ b_min(0), b_min(1) + size, b_min(2) });
    geo.add_vertex(Vec3f{ b_min(0), b_min(1), b_min(2) + size });
    geo.add_vertex(Vec3f{ b_max(0), b_min(1), b_min(2) });
    geo.add_vertex(Vec3f{ b_max(0) - size, b_min(1), b_min(2) });
    geo.add_vertex(Vec3f{ b_max(0), b_min(1) + size, b_min(2) });
    geo.add_vertex(Vec3f{ b_max(0), b_min(1), b_min(2) + size });
    geo.add_vertex(Vec3f{ b_max(0), b_max(1), b_min(2) });
    geo.add_vertex(Vec3f{ b_max(0) - size, b_max(1), b_min(2) });
    geo.add_vertex(Vec3f{ b_max(0), b_max(1) - size, b_min(2) });
    geo.add_vertex(Vec3f{ b_max(0), b_max(1), b_min(2) + size });
    geo.add_vertex(Vec3f{ b_min(0), b_max(1), b_min(2) });
    geo.add_vertex(Vec3f{ b_min(0) + size, b_max(1), b_min(2) });
    geo.add_vertex(Vec3f{ b_min(0), b_max(1) - size, b_min(2) });
    geo.add_vertex(Vec3f{ b_min(0), b_max(1), b_min(2) + size });
    geo.add_vertex(Vec3f{ b_min(0), b_min(1), b_max(2) });
    geo.add_vertex(Vec3f{ b_min(0) + size, b_min(1), b_max(2) });
    geo.add_vertex(Vec3f{ b_min(0), b_min(1) + size, b_max(2) });
    geo.add_vertex(Vec3f{ b_min(0), b_min(1), b_max(2) - size });
    geo.add_vertex(Vec3f{ b_max(0), b_min(1), b_max(2) });
    geo.add_vertex(Vec3f{ b_max(0) - size, b_min(1), b_max(2) });
    geo.add_vertex(Vec3f{ b_max(0), b_min(1) + size, b_max(2) });
    geo.add_vertex(Vec3f{ b_max(0), b_min(1), b_max(2) - size });
    geo.add_vertex(Vec3f{ b_max(0), b_max(1), b_max(2) });
    geo.add_vertex(Vec3f{ b_max(0) - size, b_max(1), b_max(2) });
    geo.add_vertex(Vec3f{ b_max(0), b_max(1) - size, b_max(2) });
    geo.add_vertex(Vec3f{ b_max(0), b_max(1), b_max(2) - size });
    geo.add_vertex(Vec3f{ b_min(0), b_max(1), b_max(2) });
    geo.add_vertex(Vec3f{ b_min(0) + size, b_max(1), b_max(2) });
    geo.add_vertex(Vec3f{ b_min(0), b_max(1) - size, b_max(2) });
    geo.add_vertex(Vec3f{ b_min(0), b_max(1), b_max(2) - size });

    geo.add_line(0, 1);
    geo.add_line(0, 2);
    geo.add_line(0, 3);

    geo.add_line(4, 5);
    geo.add_line(4, 6);
    geo.add_line(4, 7);

    geo.add_line(8, 9);
    geo.add_line(8, 10);
    geo.add_line(8, 11);

    geo.add_line(12, 13);
    geo.add_line(12, 14);
    geo.add_line(12, 15);

    geo.add_line(16, 17);
    geo.add_line(16, 18);
    geo.add_line(16, 19);

    geo.add_line(20, 21);
    geo.add_line(20, 22);
    geo.add_line(20, 23);

    geo.add_line(24, 25);
    geo.add_line(24, 26);
    geo.add_line(24, 27);

    geo.add_line(28, 29);
    geo.add_line(28, 30);
    geo.add_line(28, 31);

    m_bounding_box_model.init_from(std::move(geo));
}

#ifndef NDEBUG
static bool is_rotation_xy_synchronized(const Vec3d &rot_xyz_from, const Vec3d &rot_xyz_to)
{
    const Eigen::AngleAxisd angle_axis(Geometry::rotation_xyz_diff(rot_xyz_from, rot_xyz_to));
    const Vec3d  axis = angle_axis.axis();
    const double angle = angle_axis.angle();
    if (std::abs(angle) < 1e-8)
        return true;
    assert(std::abs(axis.x()) < 1e-8);
    assert(std::abs(axis.y()) < 1e-8);
    assert(std::abs(std::abs(axis.z()) - 1.) < 1e-8);
    return std::abs(axis.x()) < 1e-8 && std::abs(axis.y()) < 1e-8 && std::abs(std::abs(axis.z()) - 1.) < 1e-8;
}

static void verify_instances_rotation_synchronized(const Model &model, const GLVolumePtrs &volumes)
{
    for (int idx_object = 0; idx_object < int(model.objects.size()); ++idx_object) {
        int idx_volume_first = -1;
        for (int i = 0; i < (int)volumes.size(); ++i) {
            if (volumes[i]->object_idx() == idx_object) {
                idx_volume_first = i;
                break;
            }
        }
        //assert(idx_volume_first != -1); // object without instances?
        if (idx_volume_first == -1)
            continue;
        const Vec3d &rotation0 = volumes[idx_volume_first]->get_instance_rotation();
        for (int i = idx_volume_first + 1; i < (int)volumes.size(); ++i)
            if (volumes[i]->object_idx() == idx_object) {
                const Vec3d &rotation = volumes[i]->get_instance_rotation();
                assert(is_rotation_xy_synchronized(rotation, rotation0));
            }
    }
}
#endif /* NDEBUG */

void Selection::synchronize_unselected_instances(SyncRotationType sync_rotation_type)
{
    std::set<unsigned int> done;  // prevent processing volumes twice
    done.insert(m_list.begin(), m_list.end());

    for (unsigned int i : m_list) {
        if (done.size() == m_volumes->size())
            break;

        const GLVolume* volume = (*m_volumes)[i];
        const int object_idx = volume->object_idx();
        if (object_idx >= 1000)
            continue;

        const int instance_idx = volume->instance_idx();
        const Vec3d& rotation = volume->get_instance_rotation();
        const Vec3d& scaling_factor = volume->get_instance_scaling_factor();
        const Vec3d& mirror = volume->get_instance_mirror();

        // Process unselected instances.
        for (unsigned int j = 0; j < (unsigned int)m_volumes->size(); ++j) {
            if (done.size() == m_volumes->size())
                break;

            if (done.find(j) != done.end())
                continue;

            GLVolume* v = (*m_volumes)[j];
            if (v->object_idx() != object_idx || v->instance_idx() == instance_idx)
                continue;

            assert(is_rotation_xy_synchronized(m_cache.volumes_data[i].get_instance_rotation(), m_cache.volumes_data[j].get_instance_rotation()));
            switch (sync_rotation_type) {
            case SyncRotationType::NONE: {
                // z only rotation -> synch instance z
                // The X,Y rotations should be synchronized from start to end of the rotation.
                assert(is_rotation_xy_synchronized(rotation, v->get_instance_rotation()));
                if (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptSLA)
                    v->set_instance_offset(Z, volume->get_instance_offset().z());
                break;
            }
            case SyncRotationType::GENERAL:
                // generic rotation -> update instance z with the delta of the rotation.
                const double z_diff = Geometry::rotation_diff_z(m_cache.volumes_data[i].get_instance_rotation(), m_cache.volumes_data[j].get_instance_rotation());
                v->set_instance_rotation({ rotation.x(), rotation.y(), rotation.z() + z_diff });
                break;
            }

            v->set_instance_scaling_factor(scaling_factor);
            v->set_instance_mirror(mirror);

            done.insert(j);
        }
    }

#ifndef NDEBUG
    verify_instances_rotation_synchronized(*m_model, *m_volumes);
#endif /* NDEBUG */
}

void Selection::synchronize_unselected_volumes()
{
    for (unsigned int i : m_list) {
        const GLVolume* volume = (*m_volumes)[i];
        const int object_idx = volume->object_idx();
        if (object_idx >= 1000)
            continue;

        const int volume_idx = volume->volume_idx();
        const Vec3d& offset = volume->get_volume_offset();
        const Vec3d& rotation = volume->get_volume_rotation();
        const Vec3d& scaling_factor = volume->get_volume_scaling_factor();
        const Vec3d& mirror = volume->get_volume_mirror();

        // Process unselected volumes.
        for (unsigned int j = 0; j < (unsigned int)m_volumes->size(); ++j) {
            if (j == i)
                continue;

            GLVolume* v = (*m_volumes)[j];
            if (v->object_idx() != object_idx || v->volume_idx() != volume_idx)
                continue;

            v->set_volume_offset(offset);
            v->set_volume_rotation(rotation);
            v->set_volume_scaling_factor(scaling_factor);
            v->set_volume_mirror(mirror);
        }
    }
}

void Selection::ensure_on_bed()
{
    typedef std::map<std::pair<int, int>, double> InstancesToZMap;
    InstancesToZMap instances_min_z;

    for (size_t i = 0; i < m_volumes->size(); ++i) {
        GLVolume* volume = (*m_volumes)[i];
        if (!volume->is_wipe_tower && !volume->is_modifier &&
            std::find(m_cache.sinking_volumes.begin(), m_cache.sinking_volumes.end(), i) == m_cache.sinking_volumes.end()) {
            const double min_z = volume->transformed_convex_hull_bounding_box().min.z();
            std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
            InstancesToZMap::iterator it = instances_min_z.find(instance);
            if (it == instances_min_z.end())
                it = instances_min_z.insert(InstancesToZMap::value_type(instance, DBL_MAX)).first;

            it->second = std::min(it->second, min_z);
        }
    }

    for (GLVolume* volume : *m_volumes) {
        std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
        InstancesToZMap::iterator it = instances_min_z.find(instance);
        if (it != instances_min_z.end())
            volume->set_instance_offset(Z, volume->get_instance_offset(Z) - it->second);
    }
}

void Selection::ensure_not_below_bed()
{
    typedef std::map<std::pair<int, int>, double> InstancesToZMap;
    InstancesToZMap instances_max_z;

    for (size_t i = 0; i < m_volumes->size(); ++i) {
        GLVolume* volume = (*m_volumes)[i];
        if (!volume->is_wipe_tower && !volume->is_modifier) {
            const double max_z = volume->transformed_convex_hull_bounding_box().max.z();
            const std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
            InstancesToZMap::iterator it = instances_max_z.find(instance);
            if (it == instances_max_z.end())
                it = instances_max_z.insert({ instance, -DBL_MAX }).first;

            it->second = std::max(it->second, max_z);
        }
    }

    if (is_any_volume()) {
        for (unsigned int i : m_list) {
            GLVolume& volume = *(*m_volumes)[i];
            const std::pair<int, int> instance = std::make_pair(volume.object_idx(), volume.instance_idx());
            InstancesToZMap::const_iterator it = instances_max_z.find(instance);
            const double z_shift = SINKING_MIN_Z_THRESHOLD - it->second;
            if (it != instances_max_z.end() && z_shift > 0.0)
                volume.set_volume_offset(Z, volume.get_volume_offset(Z) + z_shift);
        }
    }
    else {
        for (GLVolume* volume : *m_volumes) {
            const std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
            InstancesToZMap::const_iterator it = instances_max_z.find(instance);
            if (it != instances_max_z.end() && it->second < SINKING_MIN_Z_THRESHOLD)
                volume->set_instance_offset(Z, volume->get_instance_offset(Z) + SINKING_MIN_Z_THRESHOLD - it->second);
        }
    }
}

bool Selection::is_from_fully_selected_instance(unsigned int volume_idx) const
{
    if (m_mode == Instance && wxGetApp().plater()->canvas3D()->get_canvas_type() == GLCanvas3D::ECanvasType::CanvasAssembleView) {
        return true;
    }
    struct SameInstance
    {
        int obj_idx;
        int inst_idx;
        GLVolumePtrs& volumes;

        SameInstance(int obj_idx, int inst_idx, GLVolumePtrs& volumes) : obj_idx(obj_idx), inst_idx(inst_idx), volumes(volumes) {}
        bool operator () (unsigned int i) { return (volumes[i]->volume_idx() >= 0) && (volumes[i]->object_idx() == obj_idx) && (volumes[i]->instance_idx() == inst_idx); }
    };

    if ((unsigned int)m_volumes->size() <= volume_idx)
        return false;

    GLVolume* volume = (*m_volumes)[volume_idx];
    int object_idx = volume->object_idx();
    if ((int)m_model->objects.size() <= object_idx)
        return false;

    unsigned int count = (unsigned int)std::count_if(m_list.begin(), m_list.end(), SameInstance(object_idx, volume->instance_idx(), *m_volumes));
    return count == (unsigned int)m_model->objects[object_idx]->volumes.size();
}

void Selection::paste_volumes_from_clipboard()
{
#ifdef _DEBUG
    check_model_ids_validity(*m_model);
#endif /* _DEBUG */

    int dst_obj_idx = get_object_idx();
    if ((dst_obj_idx < 0) || ((int)m_model->objects.size() <= dst_obj_idx))
        return;

    ModelObject* dst_object = m_model->objects[dst_obj_idx];

    int dst_inst_idx = get_instance_idx();
    if ((dst_inst_idx < 0) || ((int)dst_object->instances.size() <= dst_inst_idx))
        return;

    ModelObject* src_object = m_clipboard.get_object(0);
    if (src_object != nullptr)
    {
        ModelInstance* dst_instance = dst_object->instances[dst_inst_idx];
        BoundingBoxf3 dst_instance_bb = dst_object->instance_bounding_box(dst_inst_idx);
        Transform3d src_matrix = src_object->instances[0]->get_transformation().get_matrix_no_offset();
        Transform3d dst_matrix = dst_instance->get_transformation().get_matrix_no_offset();
        bool from_same_object = (src_object->input_file == dst_object->input_file) && src_matrix.isApprox(dst_matrix);

        // used to keep relative position of multivolume selections when pasting from another object
        BoundingBoxf3 total_bb;

        ModelVolumePtrs volumes;
        for (ModelVolume* src_volume : src_object->volumes)
        {
            ModelVolume* dst_volume = dst_object->add_volume(*src_volume);
            dst_volume->set_new_unique_id();
            if (from_same_object)
            {
//                // if the volume comes from the same object, apply the offset in world system
//                double offset = wxGetApp().plater()->canvas3D()->get_size_proportional_to_max_bed_size(0.05);
//                dst_volume->translate(dst_matrix.inverse() * Vec3d(offset, offset, 0.0));
            }
            else
            {
                // if the volume comes from another object, apply the offset as done when adding modifiers
                // see ObjectList::load_generic_subobject()
                total_bb.merge(dst_volume->mesh().bounding_box().transformed(src_volume->get_matrix()));
            }

            volumes.push_back(dst_volume);
#ifdef _DEBUG
		    check_model_ids_validity(*m_model);
#endif /* _DEBUG */
        }

        // keeps relative position of multivolume selections
        if (!from_same_object)
        {
            for (ModelVolume* v : volumes)
            {
                v->set_offset((v->get_offset() - total_bb.center()) + dst_matrix.inverse() * (Vec3d(dst_instance_bb.max(0), dst_instance_bb.min(1), dst_instance_bb.min(2)) + 0.5 * total_bb.size() - dst_instance->get_transformation().get_offset()));
            }
        }

        wxGetApp().obj_list()->paste_volumes_into_list(dst_obj_idx, volumes);
    }

#ifdef _DEBUG
    check_model_ids_validity(*m_model);
#endif /* _DEBUG */
}

void Selection::paste_objects_from_clipboard()
{
#ifdef _DEBUG
    check_model_ids_validity(*m_model);
#endif /* _DEBUG */

    std::vector<size_t> object_idxs;
    const ModelObjectPtrs& src_objects = m_clipboard.get_objects();
    PartPlate *            plate       = wxGetApp().plater()->get_partplate_list().get_curr_plate();

    //BBS: if multiple objects are selected, move them as a whole after copy
    Vec2d shift_all = {0, 0};
    Vec2f empty_cell_all = {0, 0};
    if (src_objects.size() > 1) {
        BoundingBoxf3 bbox_all;
        for (const ModelObject *src_object : src_objects) {
            BoundingBoxf3 bbox = src_object->instance_convex_hull_bounding_box(size_t(0));
            bbox_all.merge(bbox);
        }
        auto bsize = bbox_all.size();
        if (bsize.x() < bsize.y())
            shift_all = {bbox_all.size().x(), 0};
        else
            shift_all = {0, bbox_all.size().y()};
    }

    for (size_t i=0;i<src_objects.size();i++)
    {
        const ModelObject *src_object = src_objects[i];
        ModelObject* dst_object = m_model->add_object(*src_object);

        // BBS: find an empty cell to put the copied object
        BoundingBoxf3 bbox = src_object->instance_convex_hull_bounding_box(size_t(0));

        Vec3d displacement;
        bool  in_current  = plate->intersects(bbox);
        auto  start_point = in_current ? bbox.center() : plate->get_build_volume().center();
        auto  start_offset = in_current ? src_object->instances.front()->get_offset() : plate->get_build_volume().center();
        if (shift_all(0) != 0 || shift_all(1) != 0) {
            // BBS: if multiple objects are selected, move them as a whole after copy
            if (i == 0) empty_cell_all = wxGetApp().plater()->canvas3D()->get_nearest_empty_cell({start_point(0), start_point(1)}, {bbox.size()(0)+1,bbox.size()(1)+1});
            auto instance_shift = src_object->instances.front()->get_offset() - src_objects[0]->instances.front()->get_offset();
            displacement        = {shift_all.x() + empty_cell_all.x() + instance_shift.x(), shift_all.y() + empty_cell_all.y() + instance_shift.y(), start_offset(2)};
        } else {
            // BBS: if only one object is copied, find an empty cell to put it
            auto point_offset = start_offset - start_point;
            auto empty_cell   = wxGetApp().plater()->canvas3D()->get_nearest_empty_cell({start_point(0), start_point(1)}, {bbox.size()(0)+1, bbox.size()(1)+1});
            displacement      = {empty_cell.x() + point_offset.x(), empty_cell.y() + point_offset.y(), start_offset(2)};
        }

        for (ModelInstance* inst : dst_object->instances) {
            inst->set_offset(displacement);

            //BBS init asssmble transformation
            Geometry::Transformation t = inst->get_transformation();
            inst->set_assemble_transformation(t);
        }

        object_idxs.push_back(m_model->objects.size() - 1);
#ifdef _DEBUG
	    check_model_ids_validity(*m_model);
#endif /* _DEBUG */
    }

    wxGetApp().obj_list()->paste_objects_into_list(object_idxs);

#ifdef _DEBUG
    check_model_ids_validity(*m_model);
#endif /* _DEBUG */
}

void Selection::transform_instance_relative(
    GLVolume &volume, const VolumeCache &volume_data, TransformationType transformation_type, const Transform3d &transform, const Vec3d &world_pivot)
{
    assert(transformation_type.relative());

    const Geometry::Transformation &inst_trafo = volume_data.get_instance_transform();
    if (transformation_type.world()) {
        const Vec3d       inst_pivot = transformation_type.independent() && !is_from_single_instance() ? inst_trafo.get_offset() : world_pivot;
        const Transform3d trafo      = Geometry::translation_transform(inst_pivot) * transform * Geometry::translation_transform(-inst_pivot);
        volume.set_instance_transformation(trafo * inst_trafo.get_matrix());
    } else if (transformation_type.instance())
        volume.set_instance_transformation(inst_trafo.get_matrix() * transform);
    else
        assert(false);
}

void Selection::transform_volume_relative(
    GLVolume &volume, const VolumeCache &volume_data, TransformationType transformation_type, const Transform3d &transform, const Vec3d &world_pivot)
{
    assert(transformation_type.relative());

    const Geometry::Transformation &vol_trafo  = volume_data.get_volume_transform();
    const Geometry::Transformation &inst_trafo = volume_data.get_instance_transform();

    if (transformation_type.world()) {
        const Vec3d       inst_pivot            = transformation_type.independent() ? vol_trafo.get_offset() : (Vec3d) (inst_trafo.get_matrix().inverse() * world_pivot);
        const Transform3d inst_matrix_no_offset = inst_trafo.get_matrix_no_offset();
        const Transform3d trafo                 = Geometry::translation_transform(inst_pivot) * inst_matrix_no_offset.inverse() * transform * inst_matrix_no_offset *
                                  Geometry::translation_transform(-inst_pivot);
        volume.set_volume_transformation(trafo * vol_trafo.get_matrix());
    } else if (transformation_type.instance()) {
        const Vec3d       inst_pivot = transformation_type.independent() ? vol_trafo.get_offset() : (Vec3d) (inst_trafo.get_matrix().inverse() * world_pivot);
        const Transform3d trafo      = Geometry::translation_transform(inst_pivot) * transform * Geometry::translation_transform(-inst_pivot);
        volume.set_volume_transformation(trafo * vol_trafo.get_matrix());
    } else if (transformation_type.local())
        volume.set_volume_transformation(vol_trafo.get_matrix() * transform);
    else
        assert(false);
}

ModelVolume *get_selected_volume(const Selection &selection)
{
    const GLVolume *gl_volume = get_selected_gl_volume(selection);
    if (gl_volume == nullptr) return nullptr;
    const ModelObjectPtrs &objects = selection.get_model()->objects;
    return get_model_volume(*gl_volume, objects);
}

const GLVolume *get_selected_gl_volume(const Selection &selection)
{
    int object_idx = selection.get_object_idx();
    // is more object selected?
    if (object_idx == -1) return nullptr;

    const auto &list = selection.get_volume_idxs();
    // is more volumes selected?
    if (list.size() != 1) return nullptr;

    unsigned int volume_idx = *list.begin();
    return selection.get_volume(volume_idx);
}

ModelVolume *get_selected_volume(const ObjectID &volume_id, const Selection &selection)
{
    const Selection::IndicesList &volume_ids    = selection.get_volume_idxs();
    const ModelObjectPtrs &       model_objects = selection.get_model()->objects;
    for (auto id : volume_ids) {
        const GLVolume *             selected_volume = selection.get_volume(id);
        const GLVolume::CompositeID &cid             = selected_volume->composite_id;
        ModelObject *                obj             = model_objects[cid.object_id];
        ModelVolume *                volume          = obj->volumes[cid.volume_id];
        if (volume_id == volume->id()) return volume;
    }
    return nullptr;
}

ModelVolume *get_volume(const ObjectID &volume_id, const Selection &selection)
{
    const ModelObjectPtrs &objects = selection.get_model()->objects;
    for (const ModelObject *object : objects) {
        for (ModelVolume *volume : object->volumes) {
            if (volume->id() == volume_id) return volume;
        }
    }
    return nullptr;
}

} // namespace GUI
} // namespace Slic3r
