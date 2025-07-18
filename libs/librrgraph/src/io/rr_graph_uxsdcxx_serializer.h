#pragma once

#include <vector>
#include <cstring>
#include <algorithm>
#include <iostream>

#include "rr_graph_uxsdcxx_interface.h"

#include "rr_node.h"
#include "rr_graph_type.h"
#include "rr_graph_cost.h"
#include "rr_graph_view.h"
#include "rr_graph_builder.h"
#include "rr_rc_data.h"
#include "rr_metadata.h"

#include "check_rr_graph.h"
#include "read_xml_arch_file.h"

#include "device_grid.h"
#include "alloc_and_load_rr_indexed_data.h"
#include "get_parallel_segs.h"

#include "vpr_error.h"
#include "vtr_log.h"
#include "vtr_version.h"
#include "vtr_util.h"
#include "arch_util.h"
#include "physical_types_util.h"

class MetadataBind {
  public:
    MetadataBind(MetadataStorage<int>* rr_node_metadata, MetadataStorage<std::tuple<int, int, short>>* rr_edge_metadata,
                vtr::string_internment* strings, vtr::interned_string empty)
        : is_node_(false)
        , is_edge_(false)
        , ignore_(false)
        , inode_(OPEN)
        , sink_node_(OPEN)
        , switch_id_(OPEN)
        , strings_(strings)
        , name_(empty)
        , value_(empty)
        , rr_node_metadata_(rr_node_metadata)
        , rr_edge_metadata_(rr_edge_metadata) {}

    ~MetadataBind() {
        assert_clear();
        VTR_ASSERT(!is_node_);
        VTR_ASSERT(!is_edge_);
    }

    void set_name(const char* name) {
        if (!ignore_) {
            name_ = strings_->intern_string(vtr::string_view(name));
        }
    }
    void set_value(const char* value) {
        if (!ignore_) {
            value_ = strings_->intern_string(vtr::string_view(value));
        }
    }
    void set_node_target(int inode) {
        VTR_ASSERT(!is_node_);
        VTR_ASSERT(!is_edge_);

        is_node_ = true;
        inode_ = inode;
    }

    void set_edge_target(int source_node, int sink_node, int switch_id) {
        VTR_ASSERT(!is_node_);
        VTR_ASSERT(!is_edge_);

        is_edge_ = true;
        inode_ = source_node;
        sink_node_ = sink_node;
        switch_id_ = switch_id;
    }

    void set_ignore() {
        ignore_ = true;
    }

    void bind() {
        if (is_node_) {
            vpr::add_rr_node_metadata(*rr_node_metadata_, inode_, name_, value_);
        } else if (is_edge_) {
            vpr::add_rr_edge_metadata(*rr_edge_metadata_,inode_, sink_node_, switch_id_,
                                      name_,
                                      value_);
        } else if (ignore_) {
            // Do nothing.
        } else {
            VTR_ASSERT(is_node_ || is_edge_ || ignore_);
        }

        is_node_ = false;
        is_edge_ = false;
    }

    void assert_clear() {
    }

    void finish() {
        assert_clear();
        is_node_ = false;
        is_edge_ = false;
    }

  private:
    bool is_node_;
    bool is_edge_;
    bool ignore_;
    int inode_;
    int sink_node_;
    int switch_id_;
    vtr::string_internment* strings_;
    vtr::interned_string name_;
    vtr::interned_string value_;
    MetadataStorage<int>* rr_node_metadata_;
    MetadataStorage<std::tuple<int, int, short>>* rr_edge_metadata_;
};

// Context for walking metadata.
class t_metadata_dict_iterator {
  public:
    explicit t_metadata_dict_iterator(const t_metadata_dict* d, const std::function<void(const char*)>* report_error)
        : meta(d)
        , current_index(0)
        , report_error_(report_error) {}

    size_t size() {
        return meta->size();
    }

    const t_metadata_dict::value_type* advance(int n) {
        if (n != current_index) {
            (*report_error_)(vtr::string_fmt("Iterator out of sync %d != %d",
                                             n, current_index)
                                 .c_str());
        }

        current_index += 1;
        if (n == 0) {
            iter = meta->cbegin();
        } else {
            ++iter;
        }

        VTR_ASSERT(iter != meta->end());
        return &*iter;
    }

  private:
    const t_metadata_dict* meta;
    t_metadata_dict::const_iterator iter;
    int current_index;
    const std::function<void(const char*)>* report_error_;
};

class EdgeWalker {
  public:
    void initialize(const t_rr_graph_storage* nodes, const RRGraphView* rr_graph) {
        nodes_ = nodes;
        rr_graph_ = rr_graph;
        num_edges_ = 0;
        current_src_inode_ = 0;
        current_edge_ = 0;
        current_idx_ = 0;

        // TODO: Once rr_graph_storage is fully shadowed by RRGraphView, the cached nodes_ will be removed.
        for (const auto& node : *nodes) {
            num_edges_ += rr_graph_->num_edges(node.id());
        }
    }

    size_t num_edges() const {
        return num_edges_;
    }

    int current_src_node() const {
        return current_src_inode_;
    }
    int current_sink_node() const {
        VTR_ASSERT(current_src_inode_ < nodes_->size());
        return size_t(rr_graph_->edge_sink_node(RRNodeId(current_src_inode_), current_edge_));
    }
    int current_switch_id_node() const {
        VTR_ASSERT(current_src_inode_ < nodes_->size());
        return rr_graph_->edge_switch(RRNodeId(current_src_inode_), current_edge_);
    }

    size_t advance(int n) {
        VTR_ASSERT(current_idx_ < num_edges_);
        VTR_ASSERT(current_src_inode_ < nodes_->size());

        if (n > 0) {
            current_edge_ += 1;
        }

        if (current_edge_ >= rr_graph_->num_edges(RRNodeId(current_src_inode_))) {
            // Done with current_src_inode_, advance to the end of the
            // node list, or the next node with at least 1 edge.
            current_edge_ = 0;

            do {
                current_src_inode_ += 1;

                // This is the last edge, return now.
                if (current_src_inode_ == nodes_->size()) {
                    VTR_ASSERT(current_idx_ + 1 == num_edges_);
                    return current_idx_++;
                }
            } while (rr_graph_->num_edges(RRNodeId(current_src_inode_)) < 1);
        }

        VTR_ASSERT(current_src_inode_ < nodes_->size());

        return current_idx_++;
    }

  private:
    const t_rr_graph_storage* nodes_;
    const RRGraphView* rr_graph_;
    size_t num_edges_;
    size_t current_src_inode_;
    size_t current_edge_;
    size_t current_idx_;
};

struct RrGraphContextTypes : public uxsd::DefaultRrGraphContextTypes {
    using XListReadContext = int;
    using YListReadContext = int;
    using TimingReadContext = const t_rr_switch_inf*;
    using SizingReadContext = const t_rr_switch_inf*;
    using SwitchReadContext = const t_rr_switch_inf*;
    using SegmentTimingReadContext = const t_segment_inf*;
    using SegmentReadContext = const t_segment_inf*;
    using PinReadContext = const std::pair<const t_physical_tile_type*, int>;
    using PinClassReadContext = const std::pair<const t_physical_tile_type*, const t_class*>;
    using BlockTypeReadContext = const t_physical_tile_type*;
    using GridLocReadContext = const t_grid_tile*;
    using NodeLocReadContext = const t_rr_node;
    using NodeTimingReadContext = const t_rr_node;
    using NodeSegmentReadContext = const t_rr_node;
    using MetaReadContext = const t_metadata_dict::value_type*;
    using MetadataReadContext = t_metadata_dict_iterator;
    using NodeReadContext = const t_rr_node;
    using EdgeReadContext = const EdgeWalker*;
    using RrEdgesReadContext = EdgeWalker;
    using TimingWriteContext = t_rr_switch_inf*;
    using SizingWriteContext = t_rr_switch_inf*;
    using SwitchWriteContext = t_rr_switch_inf*;
    using SegmentTimingWriteContext = const t_segment_inf*;
    using SegmentWriteContext = const t_segment_inf*;
    using PinWriteContext = const std::pair<const t_physical_tile_type*, int>;
    using PinClassWriteContext = std::tuple<const t_physical_tile_type*, const t_class*, int>;
    using BlockTypeWriteContext = std::pair<const t_physical_tile_type*, int>;
    using NodeLocWriteContext = int;
    using NodeTimingWriteContext = int;
    using NodeSegmentWriteContext = int;
    using MetaWriteContext = MetadataBind;
    using MetadataWriteContext = MetadataBind;
    using NodeWriteContext = int;
    using EdgeWriteContext = MetadataBind;
};

class RrGraphSerializer final : public uxsd::RrGraphBase<RrGraphContextTypes> {
  public:
    RrGraphSerializer(
        const e_graph_type graph_type,
        const enum e_base_cost_type base_cost_type,
        int* wire_to_rr_ipin_switch,
        int* wire_to_rr_ipin_switch_between_dice,
        bool do_check_rr_graph,
        const char* read_rr_graph_name,
        std::string* loaded_rr_graph_filename,
        bool read_edge_metadata,
        bool echo_enabled,
        const char* echo_file_name,
        t_chan_width* chan_width,
        t_rr_graph_storage* rr_nodes,
        RRGraphBuilder* rr_graph_builder,
        RRGraphView* rr_graph,
        vtr::vector<RRSwitchId, t_rr_switch_inf>* rr_switch_inf,
        vtr::vector<RRIndexedDataId, t_rr_indexed_data>* rr_indexed_data,
        std::vector<t_rr_rc_data>* rr_rc_data,
        const std::vector<t_arch_switch_inf>& arch_switch_inf,
        const vtr::vector<RRSegmentId, t_segment_inf>& segment_inf,
        const std::vector<t_physical_tile_type>& physical_tile_types,
        const DeviceGrid& grid,
        MetadataStorage<int>* rr_node_metadata,
        MetadataStorage<std::tuple<int, int, short>>* rr_edge_metadata,
        vtr::string_internment* strings,
        bool is_flat)
        : wire_to_rr_ipin_switch_(wire_to_rr_ipin_switch)
        , wire_to_rr_ipin_switch_between_dice_(wire_to_rr_ipin_switch_between_dice)
        , chan_width_(chan_width)
        , rr_nodes_(rr_nodes)
        , rr_graph_builder_(rr_graph_builder)
        , rr_graph_(rr_graph)
        , rr_switch_inf_(rr_switch_inf)
        , rr_indexed_data_(rr_indexed_data)
        , loaded_rr_graph_filename_(loaded_rr_graph_filename)
        , rr_rc_data_(rr_rc_data)
        , graph_type_(graph_type)
        , base_cost_type_(base_cost_type)
        , do_check_rr_graph_(do_check_rr_graph)
        , read_rr_graph_name_(read_rr_graph_name)
        , read_edge_metadata_(read_edge_metadata)
        , echo_enabled_ (echo_enabled)
        , echo_file_name_ (echo_file_name)
        , arch_switch_inf_(arch_switch_inf)
        , segment_inf_(segment_inf)
        , physical_tile_types_(physical_tile_types)
        , grid_(grid)
        , rr_node_metadata_(rr_node_metadata)
        , rr_edge_metadata_(rr_edge_metadata)
        , strings_(strings)
        , empty_(strings_->intern_string(vtr::string_view("")))
        , report_error_(nullptr)
        , is_flat_(is_flat) {
        // Initialize internal data
        init_side_map();
        init_segment_inf_x_y();
        curr_tmp_block_type_id = -1;
        curr_tmp_height_offset = -1;
        curr_tmp_width_offset = -1;
        curr_tmp_layer = 0;
        curr_tmp_x = -1;
        curr_tmp_y = -1;
    }

    /* A truth table to help understand the conversion from VPR side mask to uxsd side code
     *
     * index | LEFT BOTTOM RIGHT TOP | mask
     * ======+======================+======
     *  1    |                    X  | 0001
     * ======+======================+======
     *  2    |               X       | 0010
     * ======+======================+======
     *  3    |               X    X  | 0011
     * ======+======================+======
     *  4    |         X             | 0100
     * ======+======================+======
     *  5    |         X          X  | 0101
     * ======+======================+======
     *  6    |         X     X       | 0110
     * ======+======================+======
     *  7    |         X     X    X  | 0111
     * ======+======================+======
     *  8    |  X                    | 1000
     * ======+======================+======
     *  9    |  X                 X  | 1001
     * ======+======================+======
     *  10   |  X            X       | 1010
     * ======+======================+======
     *  11   |  X            X    X  | 1011
     * ======+======================+======
     *  12   |  X      X             | 1100
     * ======+======================+======
     *  13   |  X      X          X  | 1101
     * ======+======================+======
     *  14   |  X      X     X       | 1110
     * ======+======================+======
     *  15   |  X      X     X    X  | 1111
     */
  private:
    virtual void init_side_map() final {
        side_map_[0] = uxsd::enum_loc_side::UXSD_INVALID;
        side_map_[(1 << TOP)] = uxsd::enum_loc_side::TOP;
        side_map_[(1 << RIGHT)] = uxsd::enum_loc_side::RIGHT;
        side_map_[(1 << BOTTOM)] = uxsd::enum_loc_side::BOTTOM;
        side_map_[(1 << LEFT)] = uxsd::enum_loc_side::LEFT;
        side_map_[(1 << TOP) | (1 << LEFT)] = uxsd::enum_loc_side::TOP_LEFT;
        side_map_[(1 << TOP) | (1 << RIGHT)] = uxsd::enum_loc_side::TOP_RIGHT;
        side_map_[(1 << TOP) | (1 << BOTTOM)] = uxsd::enum_loc_side::TOP_BOTTOM;
        side_map_[(1 << RIGHT) | (1 << BOTTOM)] = uxsd::enum_loc_side::RIGHT_BOTTOM;
        side_map_[(1 << RIGHT) | (1 << LEFT)] = uxsd::enum_loc_side::RIGHT_LEFT;
        side_map_[(1 << BOTTOM) | (1 << LEFT)] = uxsd::enum_loc_side::BOTTOM_LEFT;
        side_map_[(1 << TOP) | (1 << RIGHT) | (1 << BOTTOM)] = uxsd::enum_loc_side::TOP_RIGHT_BOTTOM;
        side_map_[(1 << TOP) | (1 << RIGHT) | (1 << LEFT)] = uxsd::enum_loc_side::TOP_RIGHT_LEFT;
        side_map_[(1 << TOP) | (1 << BOTTOM) | (1 << LEFT)] = uxsd::enum_loc_side::TOP_BOTTOM_LEFT;
        side_map_[(1 << RIGHT) | (1 << BOTTOM) | (1 << LEFT)] = uxsd::enum_loc_side::RIGHT_BOTTOM_LEFT;
        side_map_[(1 << TOP) | (1 << RIGHT) | (1 << BOTTOM) | (1 << LEFT)] = uxsd::enum_loc_side::TOP_RIGHT_BOTTOM_LEFT;
    }

    /**
     * @brief This function separates the segments in segment_inf_ based on whether their parallel axis 
     *        is X or Y, and it stores them in segment_inf_x_ and segment_inf_y_.
     */
    void init_segment_inf_x_y(){

        /* Create a temp copy to convert from vtr::vector to std::vector
         * This is required because the ``alloc_and_load_rr_indexed_data()`` function supports only std::vector data
         * type for ``rr_segments``
         * Note that this is a dirty fix (to avoid massive code changes)
         * TODO: The ``alloc_and_load_rr_indexed_data()`` function should embrace ``vtr::vector`` for ``rr_segments``
         */
        std::vector<t_segment_inf> rr_segs;
        rr_segs.reserve(segment_inf_.size());
        for (auto& rr_seg : segment_inf_) {
            rr_segs.push_back(rr_seg);
        }

        t_unified_to_parallel_seg_index seg_index_map;
        segment_inf_x_ = get_parallel_segs(rr_segs, seg_index_map, X_AXIS);
        segment_inf_y_ = get_parallel_segs(rr_segs, seg_index_map, Y_AXIS);

    }

    /**
     * @brief Search for a segment with a matching segment ID and return its position index
     *        in the list of segments along the corresponding axis (X or Y).
     * @param segment_id The ID of the segment to search for.
     * @param axis The axis along which to search for the segment (X or Y).
     * @return int The position index of the matching segment.
     */
    int find_segment_index_along_axis(int segment_id, e_parallel_axis axis) const {
        const std::vector<t_segment_inf>* segment_inf_vec_ptr;

        if (axis == X_AXIS)
            segment_inf_vec_ptr = &segment_inf_x_;
        else
            segment_inf_vec_ptr = &segment_inf_y_;

        for(std::vector<t_segment_inf>::size_type i=0; i < (*segment_inf_vec_ptr).size(); i++){
            if((*segment_inf_vec_ptr)[i].seg_index == segment_id)
                return static_cast<int>(i);
        }

        if (axis == X_AXIS)
            VTR_LOG_ERROR("Segment ID %d not found in the list of segments along X axis.\n", segment_id);
        else
            VTR_LOG_ERROR("Segment ID %d not found in the list of segments along Y axis.\n", segment_id);
        
        return -1;
    }

  public:
    void start_load(const std::function<void(const char*)>* report_error_in) final {
        // report_error_in should be invoked if RrGraphSerializer encounters
        // an error during the read.
        report_error_ = report_error_in;
    }
    void start_write() final {}
    void finish_write() final {}

    // error_encountered will be invoked by the reader implementation whenever
    // any error is encountered.
    //
    // This method should **not** be invoked from within RrGraphSerializer,
    // instead the error should be reported via report_error.  This enables
    // the reader implementation to add context (e.g. file and line number).
    void error_encountered(const char* file, int line, const char* message) final {
        vpr_throw(VPR_ERROR_ROUTE, file, line, "%s", message);
    }

    /** Generated for complex type "timing":
     * <xs:complexType name="timing">
     *   <xs:attribute name="R" type="xs:float" />
     *   <xs:attribute name="Cin" type="xs:float" />
     *   <xs:attribute name="Cinternal" type="xs:float" />
     *   <xs:attribute name="Cout" type="xs:float" />
     *   <xs:attribute name="Tdel" type="xs:float" />
     * </xs:complexType>
     */
    inline void set_timing_Cin(float Cin, t_rr_switch_inf*& sw) final {
        sw->Cin = Cin;
    }
    inline float get_timing_Cin(const t_rr_switch_inf*& sw) final {
        return sw->Cin;
    }

    inline void set_timing_Cinternal(float Cinternal, t_rr_switch_inf*& sw) final {
        sw->Cinternal = Cinternal;
    }
    inline float get_timing_Cinternal(const t_rr_switch_inf*& sw) final {
        return sw->Cinternal;
    }

    inline void set_timing_Cout(float Cout, t_rr_switch_inf*& sw) final {
        sw->Cout = Cout;
    }
    inline float get_timing_Cout(const t_rr_switch_inf*& sw) final {
        return sw->Cout;
    }

    inline void set_timing_R(float R, t_rr_switch_inf*& sw) final {
        sw->R = R;
    }
    inline float get_timing_R(const t_rr_switch_inf*& sw) final {
        return sw->R;
    }

    inline void set_timing_Tdel(float Tdel, t_rr_switch_inf*& sw) final {
        sw->Tdel = Tdel;
    }
    inline float get_timing_Tdel(const t_rr_switch_inf*& sw) final {
        return sw->Tdel;
    }

    /** Generated for complex type "switch":
     * <xs:complexType name="switch">
     *   <xs:all>
     *     <xs:element minOccurs="0" name="timing" type="timing" />
     *     <xs:element name="sizing" type="sizing" />
     *   </xs:all>
     *   <xs:attribute name="id" type="xs:int" use="required" />
     *   <xs:attribute name="name" type="xs:string" use="required" />
     *   <xs:attribute name="type" type="switch_type" />
     * </xs:complexType>
     */
    inline void set_switch_name(const char* name, t_rr_switch_inf*& sw) final {
        // Switch names are required to be allocated in the architecture,
        // so search the arch_switch_inf_ for the relevant name, and set the
        // rr graph switch structure with that copy of the string.
        //
        // If the switch name is not present in the architecture, generate an
        // error.
        // If the graph is written when flat-routing is enabled, the types of the switches inside of the rr_graph are also
        // added to the XML file. These types are not added to the data structure that contain arch switch types. They are added to all_sw_inf under device context.
        // It remains as a future work to remove the arch_switch_types and use all_sw info under device_ctx instead.
        bool found_arch_name = false;
        std::string string_name = std::string(name);
        // The string name has the format of "Internal Switch/delay". So, I have to use compare to specify the portion I want to be compared.
        bool is_internal_sw = string_name.compare(0, strlen(VPR_INTERNAL_SWITCH_NAME), VPR_INTERNAL_SWITCH_NAME) == 0;
        for (const auto& arch_sw_inf: arch_switch_inf_) {
            if (string_name == arch_sw_inf.name || is_internal_sw) {
                found_arch_name = true;
                break;
            }
        }
        if (!found_arch_name) {
            VTR_LOG("Switch name '%s' found in RR graph input from file but not in the architecture file; creating it.\n", string_name.c_str());
        }
        sw->intra_tile = is_internal_sw;
        sw->name = string_name;
    }
    inline const char* get_switch_name(const t_rr_switch_inf*& sw) final {
        return sw->name.c_str();
    }

    inline void set_switch_type(uxsd::enum_switch_type type, t_rr_switch_inf*& sw) final {
        sw->set_type(from_uxsd_switch_type(type));
    }
    inline uxsd::enum_switch_type get_switch_type(const t_rr_switch_inf*& sw) final {
        return to_uxsd_switch_type(sw->type());
    }

    inline t_rr_switch_inf* init_switch_timing(t_rr_switch_inf*& sw) final {
        return sw;
    }
    inline void finish_switch_timing(t_rr_switch_inf*& /*sw*/) final {}

    inline const t_rr_switch_inf* get_switch_timing(const t_rr_switch_inf*& sw) final {
        return sw;
    }
    inline bool has_switch_timing(const t_rr_switch_inf*& /*sw*/) final {
        return true;
    }

    inline t_rr_switch_inf* init_switch_sizing(t_rr_switch_inf*& sw, float buf_size, float mux_trans_size) final {
        sw->buf_size = buf_size;
        sw->mux_trans_size = mux_trans_size;
        return sw;
    }
    inline void finish_switch_sizing(t_rr_switch_inf*& /*sw*/) final {}
    inline const t_rr_switch_inf* get_switch_sizing(const t_rr_switch_inf*& sw) final {
        return sw;
    }

    inline float get_sizing_buf_size(const t_rr_switch_inf*& sw) final {
        return sw->buf_size;
    }
    inline float get_sizing_mux_trans_size(const t_rr_switch_inf*& sw) final {
        return sw->mux_trans_size;
    }

    /** Generated for complex type "switches":
     * <xs:complexType name="switches">
     *   <xs:sequence>
     *     <xs:element maxOccurs="unbounded" name="switch" type="switch" />
     *   </xs:sequence>
     * </xs:complexType>
     */
    inline void preallocate_switches_switch(void*& /*ctx*/, size_t size) final {
        rr_switch_inf_->reserve(size);
    }

    inline t_rr_switch_inf* add_switches_switch(void*& /*ctx*/, int id) final {
        // make_room_in_vector will not allocate if preallocate_switches_switch
        // was invoked, but on formats that lack size on read,
        // make_room_in_vector will use an allocation pattern that is
        // amoritized O(1).
        make_room_in_vector(rr_switch_inf_, id);

        (*rr_switch_inf_)[RRSwitchId(id)].R = 0;
        (*rr_switch_inf_)[RRSwitchId(id)].Cin = 0;
        (*rr_switch_inf_)[RRSwitchId(id)].Cout = 0;
        (*rr_switch_inf_)[RRSwitchId(id)].Cinternal = 0;
        (*rr_switch_inf_)[RRSwitchId(id)].Tdel = 0;

        return &(*rr_switch_inf_)[RRSwitchId(id)];
    }
    inline void finish_switches_switch(t_rr_switch_inf*& /*ctx*/) final {}
    inline size_t num_switches_switch(void*& /*ctx*/) final {
        return rr_switch_inf_->size();
    }
    inline const t_rr_switch_inf* get_switches_switch(int n, void*& /*ctx*/) final {
        return &(*rr_switch_inf_)[RRSwitchId(n)];
    }

    inline int get_switch_id(const t_rr_switch_inf*& sw) final {
        return sw - &(*rr_switch_inf_)[RRSwitchId(0)];
    }

    inline void* init_rr_graph_switches(void*& /*ctx*/) final {
        return nullptr;
    }
    inline void finish_rr_graph_switches(void*& /*ctx*/) final {
        // If make_room_in_vector was used for allocation, this ensures that
        // the final storage has no overhead.
        rr_switch_inf_->shrink_to_fit();
    }

    /** Generated for complex type "meta":
     * <xs:complexType name="meta">
     *   <xs:simpleContent>
     *     <xs:extension base="xs:string">
     *       <xs:attribute name="name" type="xs:string" use="required" />
     *     </xs:extension>
     *   </xs:simpleContent>
     * </xs:complexType>
     */
    inline void set_meta_name(const char* name, MetadataBind& bind) final {
        bind.set_name(name);
    }

  private:
    std::string temp_;

  public:
    inline const char* get_meta_name(const t_metadata_dict::value_type*& meta_value) final {
        meta_value->first.get(strings_, &temp_);
        return temp_.c_str();
    }

    inline void set_meta_value(const char* value, MetadataBind& bind) final {
        bind.set_value(value);
    }
    inline const char* get_meta_value(const t_metadata_dict::value_type*& meta_value) final {
        VTR_ASSERT(meta_value->second.size() == 1);
        meta_value->second[0].as_string().get(strings_, &temp_);
        return temp_.c_str();
    }

    /** Generated for complex type "metadata":
     * <xs:complexType name="metadata">
     *   <xs:sequence>
     *     <xs:element maxOccurs="unbounded" name="meta" type="meta" />
     *   </xs:sequence>
     * </xs:complexType>
     */
    inline void preallocate_metadata_meta(MetadataBind& /*bind*/, size_t /*size*/) final {
    }
    inline MetadataBind add_metadata_meta(MetadataBind& bind) final {
        return bind;
    }
    inline void finish_metadata_meta(MetadataBind& bind) final {
        bind.bind();
    }

    inline size_t num_metadata_meta(t_metadata_dict_iterator& itr) final {
        return itr.size();
    }
    inline const t_metadata_dict::value_type* get_metadata_meta(int n, t_metadata_dict_iterator& itr) final {
        return itr.advance(n);
    }

    /** Generated for complex type "node_loc":
     * <xs:complexType name="node_loc">
     *   <xs:attribute name="xlow" type="xs:int" use="required" />
     *   <xs:attribute name="ylow" type="xs:int" use="required" />
     *   <xs:attribute name="xhigh" type="xs:int" use="required" />
     *   <xs:attribute name="yhigh" type="xs:int" use="required" />
     *   <xs:attribute name="side" type="loc_side" />
     *   <xs:attribute name="ptc" type="xs:int" use="required" />
     * </xs:complexType>
     */

    inline int init_node_loc(int& inode, int ptc, int xhigh, int xlow, int yhigh, int ylow) final {
        auto node = (*rr_nodes_)[inode];
        RRNodeId node_id = node.id();

        rr_graph_builder_->set_node_coordinates(node_id, xlow, ylow, xhigh, yhigh);
        // We set the layer num 0 - If it is specified in the XML, it will be overwritten
        rr_graph_builder_->set_node_layer(node_id, 0);
        rr_graph_builder_->set_node_ptc_num(node_id, ptc);
        return inode;
    }
    inline void finish_node_loc(int& /*inode*/) final {}
    inline const t_rr_node get_node_loc(const t_rr_node& node) final {
        return node;
    }

    inline int get_node_loc_ptc(const t_rr_node& node) final {
        return rr_graph_->node_ptc_num(node.id());
    }
    inline int get_node_loc_layer(const t_rr_node& node) final {
        return rr_graph_->node_layer(node.id());
    }
    inline int get_node_loc_xhigh(const t_rr_node& node) final {
        return rr_graph_->node_xhigh(node.id());
    }
    inline int get_node_loc_xlow(const t_rr_node& node) final {
        return rr_graph_->node_xlow(node.id());
    }
    inline int get_node_loc_yhigh(const t_rr_node& node) final {
        return rr_graph_->node_yhigh(node.id());
    }
    inline int get_node_loc_ylow(const t_rr_node& node) final {
        return rr_graph_->node_ylow(node.id());
    }

    inline void set_node_loc_layer(int layer_num, int& inode) final {
        auto node = (*rr_nodes_)[inode];
        RRNodeId node_id = node.id();


        VTR_ASSERT(layer_num >= 0);
        rr_graph_builder_->set_node_layer(node_id, layer_num);
    }

    inline void set_node_loc_side(uxsd::enum_loc_side side, int& inode) final {
        auto node = (*rr_nodes_)[inode];
        RRNodeId node_id = node.id();
        const auto& rr_graph = (*rr_graph_);

        if (uxsd::enum_loc_side::UXSD_INVALID == side) {
            // node_loc.side is only expected on IPIN/OPIN.
            if (rr_graph.node_type(node.id()) == e_rr_type::IPIN || rr_graph.node_type(node.id()) == e_rr_type::OPIN) {
                report_error(
                    "inode %d is type %d, which requires a side, but no side was supplied.",
                    inode, rr_graph.node_type(node.id()));
            }
        } else {
            std::bitset<NUM_2D_SIDES> sides_to_add = from_uxsd_loc_side(side);
            for (const e_side& side_to_add : TOTAL_2D_SIDES) {
                if (sides_to_add[side_to_add]) {
                    rr_graph_builder_->add_node_side(node_id, side_to_add);
                }
            }
        }
    }

    inline uxsd::enum_loc_side get_node_loc_side(const t_rr_node& node) final {
        const auto& rr_graph = (*rr_graph_);
        if (rr_graph.node_type(node.id()) == e_rr_type::IPIN || rr_graph.node_type(node.id()) == e_rr_type::OPIN) {
            std::bitset<NUM_2D_SIDES> sides_bitset;
            for (const e_side& side : TOTAL_2D_SIDES) {
                if (rr_graph.is_node_on_specific_side(node.id(), side)) {
                    sides_bitset.set(side);
                }
            }
            return to_uxsd_loc_side(sides_bitset);
        } else {
            return uxsd::enum_loc_side::UXSD_INVALID;
        }
    }

    /** Generated for complex type "node_timing":
     * <xs:complexType name="node_timing">
     *   <xs:attribute name="R" type="xs:float" use="required" />
     *   <xs:attribute name="C" type="xs:float" use="required" />
     * </xs:complexType>
     */
    inline int init_node_timing(int& inode, float C, float R) final {
        auto node = (*rr_nodes_)[inode];
        RRNodeId node_id = node.id();
        rr_graph_builder_->set_node_rc_index(node_id, NodeRCIndex(find_create_rr_rc_data(R, C, *rr_rc_data_)));
        return inode;
    }
    inline void finish_node_timing(int& /*inode*/) final {}
    inline const t_rr_node get_node_timing(const t_rr_node& node) final {
        return node;
    }
    inline bool has_node_timing(const t_rr_node& /*node*/) final {
        return true;
    }

    inline float get_node_timing_C(const t_rr_node& node) final {
        return rr_graph_->node_C(node.id());
    }
    inline float get_node_timing_R(const t_rr_node& node) final {
        return rr_graph_->node_R(node.id());
    }

    /** Generated for complex type "node_segment":
     * <xs:complexType name="node_segment">
     *   <xs:attribute name="segment_id" type="xs:int" use="required" />
     * </xs:complexType>
     */
    inline int init_node_segment(int& inode, int segment_id) final {
        const auto& rr_graph = (*rr_graph_);
        if (segment_id > (ssize_t)segment_inf_.size()) {
            report_error(
                "Specified segment %d is larger than number of known segments %zu",
                segment_inf_.size());
        }

        auto node = (*rr_nodes_)[inode];
        RRNodeId node_id = node.id();

        if (e_graph_type::GLOBAL == graph_type_) {
            rr_graph_builder_->set_node_cost_index(node_id, RRIndexedDataId(0));
        } else if (rr_graph.node_type(node.id()) == e_rr_type::CHANX) {
            int seg_ind_x = find_segment_index_along_axis(segment_id, X_AXIS);
            rr_graph_builder_->set_node_cost_index(node_id, RRIndexedDataId(CHANX_COST_INDEX_START + seg_ind_x));
            seg_index_[rr_graph.node_cost_index(node.id())] = segment_id;
        } else if (rr_graph.node_type(node.id()) == e_rr_type::CHANY) {
            int seg_ind_y = find_segment_index_along_axis(segment_id, Y_AXIS);
            rr_graph_builder_->set_node_cost_index(node_id, RRIndexedDataId(CHANX_COST_INDEX_START + segment_inf_x_.size() + seg_ind_y));
            seg_index_[rr_graph.node_cost_index(node.id())] = segment_id;
        }
        return inode;
    }
    inline void finish_node_segment(int& /*inode*/) final {}
    inline int get_node_segment_segment_id(const t_rr_node& node) final {
        return (*rr_indexed_data_)[(*rr_graph_).node_cost_index(node.id())].seg_index;
    }

    inline const t_rr_node get_node_segment(const t_rr_node& node) final {
        return node;
    }
    inline bool has_node_segment(const t_rr_node& node) final {
        return (*rr_indexed_data_)[(*rr_graph_).node_cost_index(node.id())].seg_index != -1;
    }

    inline MetadataBind init_node_metadata(int& inode) final {
        MetadataBind bind(rr_node_metadata_, rr_edge_metadata_, strings_, empty_);
        bind.set_node_target(inode);
        return bind;
    }
    inline void finish_node_metadata(MetadataBind& bind) final {
        bind.finish();
    }
    inline t_metadata_dict_iterator get_node_metadata(const t_rr_node& node) final {
        const auto itr = rr_node_metadata_->find(get_node_id(node));
        return t_metadata_dict_iterator(&itr->second, report_error_);
    }
    inline bool has_node_metadata(const t_rr_node& node) final {
        const auto itr = rr_node_metadata_->find(get_node_id(node));
        return itr != rr_node_metadata_->end();
    }

    /** Generated for complex type "rr_nodes":
     * <xs:complexType name="rr_nodes">
     *   <xs:choice maxOccurs="unbounded">
     *     <xs:element name="node" type="node" />
     *   </xs:choice>
     * </xs:complexType>
     */
    inline void preallocate_rr_nodes_node(void*& /*ctx*/, size_t size) final {
        rr_graph_builder_->reserve_nodes(size);
    }
    inline int add_rr_nodes_node(void*& /*ctx*/, unsigned int capacity, unsigned int id, uxsd::enum_node_type type) final {
        // make_room_in_vector will not allocate if preallocate_rr_nodes_node
        // was invoked, but on formats that lack size on read,
        // make_room_in_vector will use an allocation pattern that is
        // amoritized O(1).
        const auto& rr_graph = (*rr_graph_);
        rr_nodes_->make_room_for_node(RRNodeId(id));
        auto node = (*rr_nodes_)[id];
        RRNodeId node_id = node.id();

        rr_graph_builder_->set_node_type(node_id, from_uxsd_node_type(type));
        rr_graph_builder_->set_node_capacity(node_id, capacity);

        switch (rr_graph.node_type(node.id())) {
            case e_rr_type::CHANX:
                break;
            case e_rr_type::CHANY:
                break;
            case e_rr_type::SOURCE:
                rr_graph_builder_->set_node_cost_index(node_id, RRIndexedDataId(SOURCE_COST_INDEX));
                break;
            case e_rr_type::SINK:
                rr_graph_builder_->set_node_cost_index(node_id, RRIndexedDataId(SINK_COST_INDEX));
                break;
            case e_rr_type::OPIN:
                rr_graph_builder_->set_node_cost_index(node_id, RRIndexedDataId(OPIN_COST_INDEX));
                break;
            case e_rr_type::IPIN:
                rr_graph_builder_->set_node_cost_index(node_id, RRIndexedDataId(IPIN_COST_INDEX));
                break;
            default:
                report_error(
                    "Invalid node type %d",
                    type);
        }

        rr_graph_builder_->set_node_rc_index(node_id, NodeRCIndex(find_create_rr_rc_data(0, 0, *rr_rc_data_)));

        return id;
    }
    inline void finish_rr_nodes_node(int& inode) final {
        auto node = (*rr_nodes_)[inode];
        RRNodeId node_id = node.id();

        // At this point, all attributes for the node are loaded. Check whether the current node is included in the temporary list of 
        // clock network virtual sinks. If it is, permanently add it to the unordered map in rr_graph_storage, using the attribute
        // name as the key.
        if (clock_net_virtual_sinks.find(inode) != clock_net_virtual_sinks.end()) {
            rr_graph_builder_->set_virtual_clock_network_root_idx(node_id);
        }
    }

    inline size_t num_rr_nodes_node(void*& /*ctx*/) final {

        return rr_nodes_->size();
    }
    inline const t_rr_node get_rr_nodes_node(int n, void*& /*ctx*/) final {
        return (*rr_nodes_)[n];
    }

    inline unsigned int get_node_capacity(const t_rr_node& node) final {
        const auto& rr_graph = (*rr_graph_);
        return rr_graph.node_capacity(node.id());
    }

    inline unsigned int get_node_id(const t_rr_node& node) final {
        return size_t(node.id());
    }
    inline uxsd::enum_node_type get_node_type(const t_rr_node& node) final {
        const auto& rr_graph = (*rr_graph_);
        return to_uxsd_node_type(rr_graph.node_type(node.id()));
    }

    inline const char* get_node_name(const t_rr_node& node) final {
        const auto& rr_graph = (*rr_graph_);
        auto node_name = rr_graph.node_name(node.id());
        if(node_name)
            return node_name.value()->c_str();
        return nullptr;
    }

    inline void set_node_direction(uxsd::enum_node_direction direction, int& inode) final {
        const auto& rr_graph = (*rr_graph_);
        auto node = (*rr_nodes_)[inode];
        RRNodeId node_id = node.id();

        if (direction == uxsd::enum_node_direction::UXSD_INVALID) {
            if (rr_graph.node_type(node.id()) == e_rr_type::CHANX || rr_graph.node_type(node.id()) == e_rr_type::CHANY) {
                report_error(
                    "inode %d is type %d, which requires a direction, but no direction was supplied.",
                    inode, rr_graph.node_type(node.id()));
            }
        } else {
            rr_graph_builder_->set_node_direction(node_id, from_uxsd_node_direction(direction));
        }
    }
    inline uxsd::enum_node_direction get_node_direction(const t_rr_node& node) final {
        const auto& rr_graph = (*rr_graph_);
        if (rr_graph.node_type(node.id()) == e_rr_type::CHANX || rr_graph.node_type(node.id()) == e_rr_type::CHANY) {
            return to_uxsd_node_direction(rr_graph.node_direction(node.id()));
        } else {
            return uxsd::enum_node_direction::UXSD_INVALID;
        }
    }

    inline void set_node_name(const char * name, int& inode) final {
        if(name[0] != '\0')
        {
            // Do not store the attribute name if the string is empty
            auto node = (*rr_nodes_)[inode];
            RRNodeId node_id = node.id();
            std::string name_str(name);
            rr_graph_builder_->set_node_name(node_id, name_str);
        }

    }
    // Currently, this function only processes cases where clk_res_type=VIRTUAL_SINK
    // It temporarily stores the node ID of the virtual sink in a temporary set. Eventually, 
    //the node ID will be stored in an unordered map within rr_graph_storage, using the attribute "name"
    // as the key. Since, at this point in the code, the "name" attribute might not have been processed yet, 
    //the final storage will occur in the finish_rr_nodes_node function.
    inline void set_node_clk_res_type(uxsd::enum_node_clk_res_type clk_res_type, int& inode) final {
        if(clk_res_type == uxsd::enum_node_clk_res_type::VIRTUAL_SINK)
        {
            clock_net_virtual_sinks.insert(inode);
        }
    }
    inline uxsd::enum_node_clk_res_type get_node_clk_res_type(const t_rr_node& node) final {
        // Currently only VIRTUAL_SINK is supported as the clk_res_type
        // If the node id doesn't match the node id of the clock virtual sink
        // the function returns UXSD_INVALID
        const auto& rr_graph = (*rr_graph_);
        RRNodeId node_id = node.id();
        if (rr_graph.is_virtual_clock_network_root(node_id)) {
            return uxsd::enum_node_clk_res_type::VIRTUAL_SINK;
        }
        return uxsd::enum_node_clk_res_type::UXSD_INVALID;
    }

    inline void* init_rr_graph_rr_nodes(void*& /*ctx*/) final {
        rr_nodes_->clear();
        seg_index_.resize(CHANX_COST_INDEX_START + segment_inf_x_.size() + segment_inf_y_.size(), -1);
        return nullptr;
    }
    inline void finish_rr_graph_rr_nodes(void*& /*ctx*/) final {
        // If make_room_in_vector was used for allocation, this ensures that
        // the final storage has no overhead.
        rr_nodes_->shrink_to_fit();
    }

    /** Generated for complex type "rr_edges":
     * <xs:complexType name="rr_edges">
     *   <xs:choice maxOccurs="unbounded">
     *     <xs:element name="edge" type="edge" />
     *   </xs:choice>
     * </xs:complexType>
     */
    /** Generated for complex type "edge":
     * <xs:complexType name="edge">
     *   <xs:all>
     *     <xs:element minOccurs="0" name="metadata" type="metadata" />
     *   </xs:all>
     *   <xs:attribute name="src_node" type="xs:unsignedInt" use="required" />
     *   <xs:attribute name="sink_node" type="xs:unsignedInt" use="required" />
     *   <xs:attribute name="switch_id" type="xs:unsignedInt" use="required" />
     * </xs:complexType>
     */
    inline void preallocate_rr_edges_edge(void*& /*ctx*/, size_t size) final {
        rr_graph_builder_->reserve_edges(size);
        if (read_edge_metadata_) {
            rr_edge_metadata_->reserve(size);
        }
    }
    inline MetadataBind add_rr_edges_edge(void*& /*ctx*/, unsigned int sink_node, unsigned int src_node, unsigned int switch_id) final {
        if (src_node >= rr_nodes_->size()) {
            report_error(
                "source_node %d is larger than rr_nodes.size() %d",
                src_node, rr_nodes_->size());
        }

        MetadataBind bind(rr_node_metadata_, rr_edge_metadata_, strings_, empty_);
        if (read_edge_metadata_) {
            bind.set_edge_target(src_node, sink_node, switch_id);
        } else {
            bind.set_ignore();
        }

        // The edge ids in the rr graph file are rr edge id not architecture edge id
        rr_graph_builder_->emplace_back_edge(RRNodeId(src_node), RRNodeId(sink_node), switch_id, true);
        return bind;
    }
    inline void finish_rr_edges_edge(MetadataBind& bind) final {
        bind.finish();
    }
    inline size_t num_rr_edges_edge(EdgeWalker& walker) final {
        return walker.num_edges();
    }
    inline const EdgeWalker* get_rr_edges_edge(int n, EdgeWalker& walker) final {
        size_t cur = walker.advance(n);
        if ((ssize_t)cur != n) {
            report_error("Incorrect edge index %zu != %d", cur, n);
        }
        return &walker;
    }

    inline unsigned int get_edge_sink_node(const EdgeWalker*& walker) final {
        return walker->current_sink_node();
    }
    inline unsigned int get_edge_src_node(const EdgeWalker*& walker) final {
        return walker->current_src_node();
    }
    inline unsigned int get_edge_switch_id(const EdgeWalker*& walker) final {
        return walker->current_switch_id_node();
    }

    inline MetadataBind init_edge_metadata(MetadataBind& bind) final {
        return bind;
    }
    inline void finish_edge_metadata(MetadataBind& bind) final {
        bind.finish();
    }
    inline t_metadata_dict_iterator get_edge_metadata(const EdgeWalker*& walker) final {
        return t_metadata_dict_iterator(&rr_edge_metadata_->find(
                                                              std::make_tuple(
                                                                  walker->current_src_node(),
                                                                  walker->current_sink_node(),
                                                                  walker->current_switch_id_node()))
                                             ->second,
                                        report_error_);
    }
    inline bool has_edge_metadata(const EdgeWalker*& walker) final {
        return rr_edge_metadata_->find(
                   std::make_tuple(
                       walker->current_src_node(),
                       walker->current_sink_node(),
                       walker->current_switch_id_node()))
               != rr_edge_metadata_->end();
    }

    inline void* init_rr_graph_rr_edges(void*& /*ctx*/) final {
        return nullptr;
    }
    inline void finish_rr_graph_rr_edges(void*& /*ctx*/) final {
        /*initialize a vector that keeps track of the number of wire to ipin switches
         * There should be only one wire to ipin switch. In case there are more, make sure to
         * store the most frequent switch */
        const auto& rr_graph = (*rr_graph_);
        std::vector<int> count_for_wire_to_ipin_switches;
        count_for_wire_to_ipin_switches.resize(rr_switch_inf_->size(), 0);
        //switch for same layer Track to IPIN connection
        //first is index, second is count
        std::pair<int, int> most_frequent_switch(-1, 0);
        //switch for different layer Track to IPIN connection
        std::vector<int> count_for_wire_to_ipin_switches_between_dice;
        count_for_wire_to_ipin_switches_between_dice.resize(rr_switch_inf_->size(), 0);
        std::pair<int,int> most_frequent_switch_between_dice(-1,0);

        // Partition the rr graph edges for efficient access to
        // configurable/non-configurable edge subsets. Must be done after RR
        // switches have been allocated.
        rr_graph_builder_->mark_edges_as_rr_switch_ids();
        rr_graph_builder_->partition_edges();

        for (int source_node = 0; source_node < (ssize_t)rr_nodes_->size(); ++source_node) {
            int num_edges = rr_nodes_->num_edges(RRNodeId(source_node));
            for (int iconn = 0; iconn < num_edges; ++iconn) {
                size_t sink_node = size_t(rr_nodes_->edge_sink_node(RRNodeId(source_node), iconn));
                size_t switch_id = rr_nodes_->edge_switch(RRNodeId(source_node), iconn);
                if (sink_node >= rr_nodes_->size()) {
                    report_error(
                        "sink_node %zu is larger than rr_nodes.size() %zu",
                        sink_node, rr_nodes_->size());
                }

                if (switch_id >= rr_switch_inf_->size()) {
                    report_error(
                        "switch_id %zu is larger than num_rr_switches %zu",
                        switch_id, rr_switch_inf_->size());
                }
                auto node = (*rr_nodes_)[source_node];

                /*Keeps track of the number of the specific type of switch that connects a wire to an ipin
                 * use the pair data structure to keep the maximum*/
                if (rr_graph.node_type(node.id()) == e_rr_type::CHANX || rr_graph.node_type(node.id()) == e_rr_type::CHANY) {
                    if(rr_graph.node_type(RRNodeId(sink_node)) == e_rr_type::IPIN){
                        if (rr_graph.node_layer(RRNodeId(sink_node)) == rr_graph.node_layer(RRNodeId(source_node))) {
                            count_for_wire_to_ipin_switches[switch_id]++;
                            if (count_for_wire_to_ipin_switches[switch_id] > most_frequent_switch.second) {
                                most_frequent_switch.first = switch_id;
                                most_frequent_switch.second = count_for_wire_to_ipin_switches[switch_id];
                            }
                        }
                        else{
                            VTR_ASSERT(rr_graph.node_layer(RRNodeId(sink_node)) != rr_graph.node_layer(RRNodeId(source_node)));
                            count_for_wire_to_ipin_switches_between_dice[switch_id]++;
                            if(count_for_wire_to_ipin_switches_between_dice[switch_id] > most_frequent_switch_between_dice.second){
                                most_frequent_switch_between_dice.first = switch_id;
                                most_frequent_switch_between_dice.second = count_for_wire_to_ipin_switches_between_dice[switch_id];
                            }
                        }
                    }
                }
            }
        }

        VTR_ASSERT(wire_to_rr_ipin_switch_ != nullptr);
        *wire_to_rr_ipin_switch_ = most_frequent_switch.first;

        VTR_ASSERT(wire_to_rr_ipin_switch_between_dice_ != nullptr);
        *wire_to_rr_ipin_switch_between_dice_ = most_frequent_switch_between_dice.first;
    }

    inline EdgeWalker get_rr_graph_rr_edges(void*& /*ctx*/) final {
        EdgeWalker walker;
        walker.initialize(rr_nodes_, rr_graph_);
        return walker;
    }

    /** Generated for complex type "channels":
     * <xs:complexType name="channels">
     *   <xs:sequence>
     *     <xs:element name="channel" type="channel" />
     *     <xs:element maxOccurs="unbounded" name="x_list" type="x_list" />
     *     <xs:element maxOccurs="unbounded" name="y_list" type="y_list" />
     *   </xs:sequence>
     * </xs:complexType>
     */
    inline int get_channel_chan_width_max(void*& /*ctx*/) final {
        return chan_width_->max;
    }
    inline int get_channel_x_max(void*& /*ctx*/) final {
        return chan_width_->x_max;
    }
    inline int get_channel_x_min(void*& /*ctx*/) final {
        return chan_width_->x_min;
    }
    inline int get_channel_y_max(void*& /*ctx*/) final {
        return chan_width_->y_max;
    }
    inline int get_channel_y_min(void*& /*ctx*/) final {
        return chan_width_->y_min;
    }
    inline void* init_channels_channel(void*& /*ctx*/, int chan_width_max, int x_max, int x_min, int y_max, int y_min) final {
        chan_width_->max = chan_width_max;
        chan_width_->x_min = x_min;
        chan_width_->y_min = y_min;
        chan_width_->x_max = x_max;
        chan_width_->y_max = y_max;
        chan_width_->x_list.resize(grid_.height());
        chan_width_->y_list.resize(grid_.width());
        return nullptr;
    }
    inline void finish_channels_channel(void*& /*ctx*/) final {
    }
    inline void* get_channels_channel(void*& /*ctx*/) final {
        return nullptr;
    }

    inline void preallocate_channels_x_list(void*& /*ctx*/, size_t /*size*/) final {
    }

    inline void* add_channels_x_list(void*& /*ctx*/, unsigned int index, int info) final {
        if (index >= chan_width_->x_list.size()) {
            report_error(
                "index %d on x_list exceeds x_list size %u",
                index, chan_width_->x_list.size());
        }
        chan_width_->x_list[index] = info;
        return nullptr;
    }
    inline void finish_channels_x_list(void*& /*ctx*/) final {}

    inline unsigned int get_x_list_index(int& n) final {
        return n;
    }
    inline int get_x_list_info(int& n) final {
        return chan_width_->x_list[n];
    }
    inline size_t num_channels_x_list(void*& /*ctx*/) final {
        return chan_width_->x_list.size();
    }
    inline int get_channels_x_list(int n, void*& /*iter*/) final {
        return n;
    }

    inline void preallocate_channels_y_list(void*& /*ctx*/, size_t /*size*/) final {
    }

    inline void* add_channels_y_list(void*& /*ctx*/, unsigned int index, int info) final {
        if (index >= chan_width_->y_list.size()) {
            report_error(
                "index %d on y_list exceeds y_list size %u",
                index, chan_width_->y_list.size());
        }
        chan_width_->y_list[index] = info;

        return nullptr;
    }
    inline void finish_channels_y_list(void*& /*ctx*/) final {}

    inline unsigned int get_y_list_index(int& n) final {
        return n;
    }
    inline int get_y_list_info(int& n) final {
        return chan_width_->y_list[n];
    }
    inline size_t num_channels_y_list(void*& /*ctx*/) final {
        return chan_width_->y_list.size();
    }
    inline int get_channels_y_list(int n, void*& /*ctx*/) final {
        return n;
    }

    inline void* init_rr_graph_channels(void*& /*ctx*/) final {
        return nullptr;
    }
    inline void finish_rr_graph_channels(void*& /*ctx*/) final {
    }

    /** Generated for complex type "segment_timing":
     * <xs:complexType name="segment_timing">
     *   <xs:attribute name="R_per_meter" type="xs:float" />
     *   <xs:attribute name="C_per_meter" type="xs:float" />
     * </xs:complexType>
     */
    inline const t_segment_inf* init_segment_timing(const t_segment_inf*& segment) final {
        return segment;
    }
    inline void finish_segment_timing(const t_segment_inf*& /*segment*/) final {}

    inline const t_segment_inf* get_segment_timing(const t_segment_inf*& segment) final {
        return segment;
    }
    inline bool has_segment_timing(const t_segment_inf*& /*segment*/) final {
        return true;
    }

    inline float get_segment_timing_C_per_meter(const t_segment_inf*& segment) final {
        return segment->Cmetal;
    }
    inline void set_segment_timing_C_per_meter(float C_per_meter, const t_segment_inf*& segment) final {
        if (segment->Cmetal != C_per_meter) {
            report_error(
                "Architecture file does not match RR graph's segment C_per_meter");
        }
    }

    inline float get_segment_timing_R_per_meter(const t_segment_inf*& segment) final {
        return segment->Rmetal;
    }
    inline void set_segment_timing_R_per_meter(float R_per_meter, const t_segment_inf*& segment) final {
        if (segment->Rmetal != R_per_meter) {
            report_error(
                "Architecture file does not match RR graph's segment R_per_meter");
        }
    }

    /** Generated for complex type "segment":
     * <xs:complexType name="segment">
     *   <xs:all>
     *     <xs:element minOccurs="0" name="timing" type="segment_timing" />
     *   </xs:all>
     *   <xs:attribute name="id" type="xs:int" use="required" />
     *   <xs:attribute name="name" type="xs:string" use="required" />
     * </xs:complexType>
     */
    inline int get_segment_id(const t_segment_inf*& segment) final {
        return segment - &segment_inf_.at(RRSegmentId(0));
    }
    inline const char* get_segment_name(const t_segment_inf*& segment) final {
        return segment->name.c_str();
    }
    inline int get_segment_length(const t_segment_inf*& segment) final {
        return segment->length;
    }
    inline void set_segment_name(const char* name, const t_segment_inf*& segment) final {
        if (segment->name != name) {
            report_error(
                "Architecture file does not match RR graph's segment name: arch uses %s, RR graph uses %s",
                segment->name.c_str(), name);
        }
    }
    inline void set_segment_length(int length, const t_segment_inf*& segment) final {
        if (segment->length != length) {
            report_error(
                "Architecture file does not match RR graph's length: arch uses %d, RR graph uses %d",
                segment->length, length);
        }
    }
    inline uxsd::enum_segment_res_type get_segment_res_type(const t_segment_inf*& segment) final {
        return to_uxsd_segment_res_type(segment->res_type);
    }
    inline void set_segment_res_type(uxsd::enum_segment_res_type seg_res_type, const t_segment_inf*& segment) final {
        if (segment->res_type != from_uxsd_segment_res_type(seg_res_type)) {
            const auto arch_index = static_cast<size_t>(segment->res_type);
            const auto rrgraph_index = static_cast<size_t>(from_uxsd_segment_res_type(seg_res_type));
            
            report_error(
                "Architecture file does not match RR graph's segment res_type: arch uses %s, RR graph uses %s",
               RES_TYPE_STRING[arch_index], RES_TYPE_STRING[rrgraph_index]);
        }
    }

    /** Generated for complex type "segments":
     * <xs:complexType name="segments">
     *   <xs:sequence>
     *     <xs:element maxOccurs="unbounded" name="segment" type="segment" />
     *   </xs:sequence>
     * </xs:complexType>
     */
    inline void preallocate_segments_segment(void*& /*ctx*/, size_t size) final {
        if (size != segment_inf_.size()) {
            report_error(
                "Architecture contains %zu segments and rr graph contains %zu segments",
                segment_inf_.size(), size);
        }
    }
    inline const t_segment_inf* add_segments_segment(void*& /*ctx*/, int id) final {
        return &segment_inf_.at(RRSegmentId(id));
    }
    inline void finish_segments_segment(const t_segment_inf*& /*iter*/) final {}
    inline size_t num_segments_segment(void*& /*iter*/) final {
        return segment_inf_.size();
    }
    inline const t_segment_inf* get_segments_segment(int n, void*& /*ctx*/) final {
        return &segment_inf_.at(RRSegmentId(n));
    }

    inline void* init_rr_graph_segments(void*& /*ctx*/) final {
        return nullptr;
    }
    inline void finish_rr_graph_segments(void*& /*ctx*/) final {
    }

    /** Generated for complex type "pin":
     * <xs:complexType name="pin">
     *   <xs:simpleContent>
     *     <xs:extension base="xs:string">
     *       <xs:attribute name="ptc" type="xs:int" use="required" />
     *     </xs:extension>
     *   </xs:simpleContent>
     * </xs:complexType>
     */

    inline void set_pin_value(const char* value, const std::pair<const t_physical_tile_type*, int>& context) final {
        const t_physical_tile_type* tile;
        int ptc;
        std::tie(tile, ptc) = context;
        if (block_type_pin_index_to_name(tile, ptc, is_flat_) != value) {
            report_error(
                "Architecture file does not match RR graph's block pin list");
        }
    }
    inline void finish_pin_class_pin(const std::pair<const t_physical_tile_type*, int>& /*ctx*/) final {
    }

    /** Generated for complex type "pin_class":
     * <xs:complexType name="pin_class">
     *   <xs:sequence>
     *     <xs:element maxOccurs="unbounded" name="pin" type="pin" />
     *   </xs:sequence>
     *   <xs:attribute name="type" type="pin_type" use="required" />
     * </xs:complexType>
     */
    inline void preallocate_pin_class_pin(std::tuple<const t_physical_tile_type*, const t_class*, int>& context, size_t size) final {
        const t_physical_tile_type* tile;
        const t_class* class_inf;
        std::tie(tile, class_inf, std::ignore) = context;
        auto class_idx = class_inf - &tile->class_inf[0];

        if (class_inf->num_pins != (ssize_t)size) {
            report_error(
                "Incorrect number of pins (%zu != %u) in %zu pin_class in block %s",
                size, class_inf->num_pins,
                class_idx, tile->name.c_str());
        }
    }
    inline const std::pair<const t_physical_tile_type*, int> add_pin_class_pin(std::tuple<const t_physical_tile_type*, const t_class*, int>& context, int ptc) final {
        const t_physical_tile_type* tile;
        const t_class* class_inf;
        std::tie(tile, class_inf, std::ignore) = context;

        // Count number of pins on this pin class.
        int& pin_count = std::get<2>(context);
        pin_count += 1;
        return std::make_pair(tile, ptc);
    }
    inline void finish_block_type_pin_class(std::tuple<const t_physical_tile_type*, const t_class*, int>& context) final {
        const t_physical_tile_type* tile;
        const t_class* class_inf;
        int pin_count;
        std::tie(tile, class_inf, pin_count) = context;
        auto class_idx = class_inf - &tile->class_inf[0];
        if (class_inf->num_pins != pin_count) {
            report_error(
                "Incorrect number of pins (%zu != %u) in %zu pin_class in block %s",
                pin_count, class_inf->num_pins,
                class_idx, tile->name.c_str());
        }
    }

    inline uxsd::enum_pin_type get_pin_class_type(const std::pair<const t_physical_tile_type*, const t_class*>& context) final {
        return to_uxsd_pin_type(context.second->type);
    }
    inline size_t num_pin_class_pin(const std::pair<const t_physical_tile_type*, const t_class*>& context) final {
        return context.second->num_pins;
    }

    inline const char* get_pin_value(const std::pair<const t_physical_tile_type*, int>& context) final {
        const t_physical_tile_type* tile;
        int ptc;
        std::tie(tile, ptc) = context;
        temp_string_ = block_type_pin_index_to_name(tile, ptc, is_flat_);
        return temp_string_.c_str();
    }
    inline int get_pin_ptc(const std::pair<const t_physical_tile_type*, int>& context) final {
        const t_physical_tile_type* tile;
        int ptc;
        std::tie(tile, ptc) = context;

        return ptc;
    }
    inline const std::pair<const t_physical_tile_type*, int> get_pin_class_pin(int n, const std::pair<const t_physical_tile_type*, const t_class*>& context) final {
        const t_physical_tile_type* tile;
        const t_class* class_inf;
        std::tie(tile, class_inf) = context;
        return std::make_pair(tile, class_inf->pinlist[n]);
    }

    inline int get_block_type_height(const t_physical_tile_type*& tile) final {
        return tile->height;
    }
    inline int get_block_type_id(const t_physical_tile_type*& tile) final {
        return tile->index;
    }
    inline const char* get_block_type_name(const t_physical_tile_type*& tile) final {
        return tile->name.c_str();
    }
    inline int get_block_type_width(const t_physical_tile_type*& tile) final {
        return tile->width;
    }
    inline size_t num_block_type_pin_class(const t_physical_tile_type*& tile) final {
        return (int)tile->class_inf.size();
    }
    inline const std::pair<const t_physical_tile_type*, const t_class*> get_block_type_pin_class(int n, const t_physical_tile_type*& tile) final {
        return std::make_pair(tile, &tile->class_inf[n]);
    }

    /** Generated for complex type "block_type":
     * <xs:complexType name="block_type">
     *   <xs:sequence>
     *     <xs:element maxOccurs="unbounded" minOccurs="0" name="pin_class" type="pin_class" />
     *   </xs:sequence>
     *   <xs:attribute name="id" type="xs:int" use="required" />
     *   <xs:attribute name="name" type="xs:string" use="required" />
     *   <xs:attribute name="width" type="xs:int" use="required" />
     *   <xs:attribute name="height" type="xs:int" use="required" />
     * </xs:complexType>
     */
    inline void set_block_type_name(const char* name, std::pair<const t_physical_tile_type*, int>& context) final {
        const t_physical_tile_type* tile = context.first;
        if (tile->name != name) {
            report_error(
                "Architecture file does not match RR graph's block name: arch uses name %s, RR graph uses name %s",
                tile->name.c_str(), name);
        }
    }

    /** Generated for complex type "block_types":
     * <xs:complexType name="block_types">
     *   <xs:sequence>
     *     <xs:element maxOccurs="unbounded" name="block_type" type="block_type" />
     *   </xs:sequence>
     * </xs:complexType>
     */
    void preallocate_block_types_block_type(void*& /*ctx*/, size_t size) final {
        if (physical_tile_types_.size() != size) {
            report_error(
                "Architecture defines %zu block types, but rr graph contains %zu block types.",
                physical_tile_types_.size(), size);
        }
    }
    inline std::pair<const t_physical_tile_type*, int> add_block_types_block_type(void*& /*ctx*/, int height, int id, int width) final {
        const auto& block_info = physical_tile_types_.at(id);
        if (block_info.width != width) {
            report_error(
                "Architecture file does not match RR graph's block width");
        }
        if (block_info.height != height) {
            report_error(
                "Architecture file does not match RR graph's block height");
        }

        // Going to count how many classes are found.
        return std::make_pair(&block_info, 0);
    }
    inline void preallocate_block_type_pin_class(std::pair<const t_physical_tile_type*, int>& context, size_t size) final {
        const t_physical_tile_type* tile = context.first;
        if ((int)tile->class_inf.size() != (ssize_t)size) {
            report_error("Architecture file does not match block type");
        }
    }

    inline std::tuple<const t_physical_tile_type*, const t_class*, int> add_block_type_pin_class(std::pair<const t_physical_tile_type*, int>& context, uxsd::enum_pin_type type) final {
        const t_physical_tile_type* tile = context.first;
        int& num_classes = context.second;

        // Count number of pin classes
        if (num_classes >= (int)tile->class_inf.size()) {
            report_error("Architecture file does not match block type");
        }
        const t_class* class_inf = &context.first->class_inf[num_classes++];

        if (class_inf->type != from_uxsd_pin_type(type)) {
            report_error(
                "Architecture file does not match RR graph's block type");
        }

        return std::make_tuple(tile, class_inf, 0);
    }
    inline void finish_block_types_block_type(std::pair<const t_physical_tile_type*, int>& context) final {
        const t_physical_tile_type* tile = context.first;
        int num_classes = context.second;
        if ((int)tile->class_inf.size() != num_classes) {
            report_error("Architecture file does not match block type");
        }
    }

    inline size_t num_block_types_block_type(void*& /*ctx*/) final {
        return physical_tile_types_.size();
    }
    inline const t_physical_tile_type* get_block_types_block_type(int n, void*& /*ctx*/) final {
        return &physical_tile_types_[n];
    }
    inline void* init_rr_graph_block_types(void*& /*ctx*/) final {
        return nullptr;
    }
    inline void finish_rr_graph_block_types(void*& /*ctx*/) final {
    }

    /** Generated for complex type "grid_loc":
     * <xs:complexType name="grid_loc">
     *   <xs:attribute name="x" type="xs:int" use="required" />
     *   <xs:attribute name="y" type="xs:int" use="required" />
     *   <xs:attribute name="block_type_id" type="xs:int" use="required" />
     *   <xs:attribute name="width_offset" type="xs:int" use="required" />
     *   <xs:attribute name="height_offset" type="xs:int" use="required" />
     * </xs:complexType>
     */
    /** Generated for complex type "grid_locs":
     * <xs:complexType name="grid_locs">
     *   <xs:sequence>
     *     <xs:element maxOccurs="unbounded" name="grid_loc" type="grid_loc" />
     *   </xs:sequence>
     * </xs:complexType>
     */
    inline void preallocate_grid_locs_grid_loc(void*& /*ctx*/, size_t size) final {
        if (grid_.grid_size() != size) {
            report_error(
                "Architecture file contains %zu grid locations, rr graph contains %zu grid locations.",
                grid_.grid_size(), size);
        }
    }
    inline void* add_grid_locs_grid_loc(void*& /*ctx*/, int block_type_id, int height_offset, int width_offset, int x, int y) final {
        curr_tmp_block_type_id = block_type_id;
        curr_tmp_height_offset = height_offset;
        curr_tmp_width_offset = width_offset;
        curr_tmp_x = x;
        curr_tmp_y = y;

        return nullptr;
    }
    inline void finish_grid_locs_grid_loc(void*& /*ctx*/) final {
        VTR_ASSERT(curr_tmp_block_type_id >= 0);
        VTR_ASSERT(curr_tmp_height_offset >= 0);
        VTR_ASSERT(curr_tmp_width_offset >= 0);
        VTR_ASSERT(curr_tmp_layer >= 0);
        VTR_ASSERT(curr_tmp_x >= 0);
        VTR_ASSERT(curr_tmp_y >= 0);
        const auto& type = grid_.get_physical_type({curr_tmp_x, curr_tmp_y, curr_tmp_layer});
        int grid_width_offset = grid_.get_width_offset({curr_tmp_x, curr_tmp_y, curr_tmp_layer});
        int grid_height_offset = grid_.get_height_offset({curr_tmp_x, curr_tmp_y, curr_tmp_layer});

        if (type->index != curr_tmp_block_type_id) {
            report_error(
                "Architecture file does not match RR graph's block_type_id at (%d, %d): arch used ID %d, RR graph used ID %d.",
                curr_tmp_layer,
                curr_tmp_x,
                curr_tmp_y,
                (type->index),
                curr_tmp_block_type_id);
        }
        if (grid_width_offset != curr_tmp_width_offset) {
            report_error(
                "Architecture file does not match RR graph's width_offset at (%d, %d)",
                curr_tmp_layer,
                curr_tmp_x,
                curr_tmp_y);
        }

        if (grid_height_offset != curr_tmp_height_offset) {
            report_error(
                "Architecture file does not match RR graph's height_offset at (%d, %d)",
                curr_tmp_layer,
                curr_tmp_x,
                curr_tmp_y);
        }

        curr_tmp_block_type_id = -1;
        curr_tmp_height_offset = -1;
        curr_tmp_width_offset = -1;
        curr_tmp_layer = 0;
        curr_tmp_x = -1;
        curr_tmp_y = -1;
    }

    inline void* init_rr_graph_grid(void*& /*ct*/) final {
        return nullptr;
    }
    inline void finish_rr_graph_grid(void*& /*ctx*/) final {
    }

    inline int get_grid_loc_block_type_id(const t_grid_tile*& grid_loc) final {
        return grid_loc->type->index;
    }
    inline int get_grid_loc_height_offset(const t_grid_tile*& grid_loc) final {
        return grid_loc->height_offset;
    }
    inline int get_grid_loc_width_offset(const t_grid_tile*& grid_loc) final {
        return grid_loc->width_offset;
    }
    inline int get_grid_loc_x(const t_grid_tile*& grid_loc) final {
        return grid_.get_grid_loc_x(grid_loc);
    }
    inline int get_grid_loc_y(const t_grid_tile*& grid_loc) final {
        return grid_.get_grid_loc_y(grid_loc);
    }

    inline int get_grid_loc_layer(const t_grid_tile*& grid_loc) final{
        return grid_.get_grid_loc_layer(grid_loc);
    }

    inline size_t num_grid_locs_grid_loc(void*& /*iter*/) final {
        return grid_.grid_size();
    }
    inline const t_grid_tile* get_grid_locs_grid_loc(int n, void*& /*ctx*/) final {
        return grid_.get_grid_locs_grid_loc(n);
    }

    inline void set_grid_loc_layer(int layer_num, void*& /*ctx*/) final {
        curr_tmp_layer = layer_num;
    }


    /** Generated for complex type "rr_graph":
     * <xs:complexType xmlns:xs="http://www.w3.org/2001/XMLSchema">
     *     <xs:all>
     *       <xs:element name="channels" type="channels" />
     *       <xs:element name="switches" type="switches" />
     *       <xs:element name="segments" type="segments" />
     *       <xs:element name="block_types" type="block_types" />
     *       <xs:element name="grid" type="grid_locs" />
     *       <xs:element name="rr_nodes" type="rr_nodes" />
     *       <xs:element name="rr_edges" type="rr_edges" />
     *     </xs:all>
     *     <xs:attribute name="tool_name" type="xs:string" />
     *     <xs:attribute name="tool_version" type="xs:string" />
     *     <xs:attribute name="tool_comment" type="xs:string" />
     *   </xs:complexType>
     */
    inline void set_rr_graph_tool_comment(const char* tool_comment, void*& /*ctx*/) final {
        std::string correct_string = "Generated from arch file ";
        correct_string += get_arch_file_name();
        if (correct_string != tool_comment) {
            VTR_LOG("\n");
            VTR_LOG_WARN("This RR graph file is %s while your input architecture file is %s, compatibility issues may arise\n",
                         tool_comment, get_arch_file_name());
            VTR_LOG("\n");
        }
    }
    inline void set_rr_graph_tool_name(const char* /*tool_name*/, void*& /*ctx*/) final {
    }
    inline void set_rr_graph_tool_version(const char* tool_version, void*& /*ctx*/) final {
        if (strcmp(tool_version, vtr::VERSION) != 0) {
            VTR_LOG("\n");
            VTR_LOG_WARN("This architecture version is for VPR %s while your current VPR version is %s, compatibility issues may arise\n",
                         tool_version, vtr::VERSION);
            VTR_LOG("\n");
        }
    }

    inline const char* get_rr_graph_tool_comment(void*& /*ctx*/) final {
        temp_string_.assign("Generated from arch file ");
        temp_string_ += get_arch_file_name();
        return temp_string_.c_str();
    }
    inline const char* get_rr_graph_tool_name(void*& /*ctx*/) final {
        return "vpr";
    }
    inline const char* get_rr_graph_tool_version(void*& /*ctx*/) final {
        return vtr::VERSION;
    }

    inline void* get_rr_graph_channels(void*& /*ctx*/) final {
        return nullptr;
    }
    inline void* get_rr_graph_switches(void*& /*ctx*/) final {
        return nullptr;
    }
    inline void* get_rr_graph_segments(void*& /*ctx*/) final {
        return nullptr;
    }
    inline void* get_rr_graph_block_types(void*& /*ctx*/) final {
        return nullptr;
    }
    inline void* get_rr_graph_grid(void*& /*ctx*/) final {
        return nullptr;
    }
    inline void* get_rr_graph_rr_nodes(void*& /*ctx*/) final {
        return nullptr;
    }

    void finish_load() final {
        process_rr_node_indices();

        rr_graph_builder_->init_fan_in();
        
        std::vector<t_segment_inf> temp_rr_segs;
        temp_rr_segs.reserve(segment_inf_.size());
        for (auto& rr_seg : segment_inf_) {
            temp_rr_segs.push_back(rr_seg);
        }

        alloc_and_load_rr_indexed_data(
            *rr_graph_,
            grid_,
            temp_rr_segs,
            segment_inf_x_,
            segment_inf_y_,
            *rr_indexed_data_,
            *wire_to_rr_ipin_switch_,
            base_cost_type_,
            echo_enabled_,
            echo_file_name_);

        VTR_ASSERT(rr_indexed_data_->size() == seg_index_.size());
        for (size_t i = 0; i < seg_index_.size(); ++i) {
            (*rr_indexed_data_)[RRIndexedDataId(i)].seg_index = seg_index_[RRIndexedDataId(i)];
        }

        VTR_ASSERT(loaded_rr_graph_filename_ != nullptr);
        VTR_ASSERT(read_rr_graph_name_ != nullptr);
        loaded_rr_graph_filename_->assign(read_rr_graph_name_);

        if (do_check_rr_graph_) {
            check_rr_graph(*rr_graph_,
                           physical_tile_types_,
                           *rr_indexed_data_,
                           grid_,
                           *chan_width_,
                           graph_type_,
                           is_flat_);
        }
    }

  private:
    /*Allocates and load the rr_node look up table. SINK and SOURCE, IPIN and OPIN
     *share the same look-up table. CHANX and CHANY have individual look-ups */
    void process_rr_node_indices() {
        auto& rr_graph_builder = (*rr_graph_builder_);

        /* Alloc the lookup table */
        for (e_rr_type rr_type : RR_TYPES) {
            rr_graph_builder.node_lookup().resize_nodes(grid_.get_num_layers(), grid_.width(), grid_.height(), rr_type, NUM_2D_SIDES);
        }

        /* Add the correct node into the vector */
        for (const t_rr_node& node : *rr_nodes_) {
            rr_graph_builder.add_node_to_all_locs(node.id());
        }
    }

    // Enum converters from/to uxsd types

    std::bitset<NUM_2D_SIDES> from_uxsd_loc_side(uxsd::enum_loc_side side) {
        std::bitset<NUM_2D_SIDES> side_mask(0x0);
        switch (side) {
            case uxsd::enum_loc_side::TOP:
                side_mask.set(TOP);
                break;
            case uxsd::enum_loc_side::TOP_LEFT:
                side_mask.set(TOP);
                side_mask.set(LEFT);
                break;
            case uxsd::enum_loc_side::TOP_RIGHT:
                side_mask.set(TOP);
                side_mask.set(RIGHT);
                break;
            case uxsd::enum_loc_side::TOP_BOTTOM:
                side_mask.set(TOP);
                side_mask.set(BOTTOM);
                break;
            case uxsd::enum_loc_side::TOP_RIGHT_LEFT:
                side_mask.set(TOP);
                side_mask.set(RIGHT);
                side_mask.set(LEFT);
                break;
            case uxsd::enum_loc_side::TOP_RIGHT_BOTTOM:
                side_mask.set(TOP);
                side_mask.set(RIGHT);
                side_mask.set(BOTTOM);
                break;
            case uxsd::enum_loc_side::TOP_BOTTOM_LEFT:
                side_mask.set(TOP);
                side_mask.set(BOTTOM);
                side_mask.set(LEFT);
                break;
            case uxsd::enum_loc_side::RIGHT:
                side_mask.set(RIGHT);
                break;
            case uxsd::enum_loc_side::RIGHT_BOTTOM:
                side_mask.set(RIGHT);
                side_mask.set(BOTTOM);
                break;
            case uxsd::enum_loc_side::RIGHT_LEFT:
                side_mask.set(RIGHT);
                side_mask.set(LEFT);
                break;
            case uxsd::enum_loc_side::RIGHT_BOTTOM_LEFT:
                side_mask.set(RIGHT);
                side_mask.set(BOTTOM);
                side_mask.set(LEFT);
                break;
            case uxsd::enum_loc_side::BOTTOM:
                side_mask.set(BOTTOM);
                break;
            case uxsd::enum_loc_side::BOTTOM_LEFT:
                side_mask.set(BOTTOM);
                side_mask.set(LEFT);
                break;
            case uxsd::enum_loc_side::LEFT:
                side_mask.set(LEFT);
                break;
            case uxsd::enum_loc_side::TOP_RIGHT_BOTTOM_LEFT:
                side_mask.set(TOP);
                side_mask.set(RIGHT);
                side_mask.set(BOTTOM);
                side_mask.set(LEFT);
                break;
            default:
                report_error(
                    "Invalid side %d", side);
        }
        return side_mask;
    }

    uxsd::enum_loc_side to_uxsd_loc_side(std::bitset<NUM_2D_SIDES> sides) {
        // Error out when
        // - the side has no valid bits
        // - the side is beyond the mapping range: this is to warn any changes on side truth table which may cause the mapping failed
        if ((0 == sides.count())
            || (sides.to_ulong() > side_map_.size() - 1)) {
            report_error(
                "Invalid side %ld", sides.to_ulong());
        }
        return side_map_[sides.to_ulong()];
    }

    Direction from_uxsd_node_direction(uxsd::enum_node_direction direction) {
        switch (direction) {
            case uxsd::enum_node_direction::INC_DIR:
                return Direction::INC;
            case uxsd::enum_node_direction::DEC_DIR:
                return Direction::DEC;
            case uxsd::enum_node_direction::BI_DIR:
                return Direction::BIDIR;
            case uxsd::enum_node_direction::NONE:
                return Direction::NONE;
            default:
                report_error(
                    "Invalid node direction %d", direction);
        }
    }

    uxsd::enum_node_direction to_uxsd_node_direction(Direction direction) {
        switch (direction) {
            case Direction::INC:
                return uxsd::enum_node_direction::INC_DIR;
            case Direction::DEC:
                return uxsd::enum_node_direction::DEC_DIR;
            case Direction::BIDIR:
                return uxsd::enum_node_direction::BI_DIR;
            case Direction::NONE:
                return uxsd::enum_node_direction::NONE;
            default:
                report_error(
                    "Invalid direction %d", direction);
        }
    }

    uxsd::enum_segment_res_type to_uxsd_segment_res_type(SegResType segment_res_type) {
        switch (segment_res_type) {
            case SegResType::GCLK:
                return uxsd::enum_segment_res_type::GCLK;
            case SegResType::GENERAL:
                return uxsd::enum_segment_res_type::GENERAL;
            default:
                report_error(
                    "Invalid segment_res_type %d", segment_res_type);
        }
    }

    SegResType from_uxsd_segment_res_type(uxsd::enum_segment_res_type segment_res_type) {
        switch (segment_res_type) {
            case uxsd::enum_segment_res_type::GCLK:
                return SegResType::GCLK;
            case uxsd::enum_segment_res_type::GENERAL:
                return SegResType::GENERAL;
            default:
                report_error(
                    "Invalid node segment_res_type %d", segment_res_type);
        }
    }

    e_rr_type from_uxsd_node_type(uxsd::enum_node_type type) {
        switch (type) {
            case uxsd::enum_node_type::CHANX:
                return e_rr_type::CHANX;
            case uxsd::enum_node_type::CHANY:
                return e_rr_type::CHANY;
            case uxsd::enum_node_type::SOURCE:
                return e_rr_type::SOURCE;
            case uxsd::enum_node_type::SINK:
                return e_rr_type::SINK;
            case uxsd::enum_node_type::OPIN:
                return e_rr_type::OPIN;
            case uxsd::enum_node_type::IPIN:
                return e_rr_type::IPIN;
            default:
                report_error(
                    "Invalid node type %d",
                    type);
        }
    }
    uxsd::enum_node_type to_uxsd_node_type(e_rr_type type) {
        switch (type) {
            case e_rr_type::CHANX:
                return uxsd::enum_node_type::CHANX;
            case e_rr_type::CHANY:
                return uxsd::enum_node_type::CHANY;
            case e_rr_type::SOURCE:
                return uxsd::enum_node_type::SOURCE;
            case e_rr_type::SINK:
                return uxsd::enum_node_type::SINK;
            case e_rr_type::OPIN:
                return uxsd::enum_node_type::OPIN;
            case e_rr_type::IPIN:
                return uxsd::enum_node_type::IPIN;
            default:
                report_error(
                    "Invalid type %d", type);
        }

        return uxsd::enum_node_type::UXSD_INVALID;
    }

    SwitchType from_uxsd_switch_type(uxsd::enum_switch_type type) {
        SwitchType switch_type = SwitchType::INVALID;
        switch (type) {
            case uxsd::enum_switch_type::TRISTATE:
                switch_type = SwitchType::TRISTATE;
                break;
            case uxsd::enum_switch_type::MUX:
                switch_type = SwitchType::MUX;
                break;
            case uxsd::enum_switch_type::PASS_GATE:
                switch_type = SwitchType::PASS_GATE;
                break;
            case uxsd::enum_switch_type::SHORT:
                switch_type = SwitchType::SHORT;
                break;
            case uxsd::enum_switch_type::BUFFER:
                switch_type = SwitchType::BUFFER;
                break;
            default:
                report_error(
                    "Invalid switch type '%d'\n", type);
        }

        return switch_type;
    }

    uxsd::enum_switch_type to_uxsd_switch_type(SwitchType type) {
        switch (type) {
            case SwitchType::TRISTATE:
                return uxsd::enum_switch_type::TRISTATE;
            case SwitchType::MUX:
                return uxsd::enum_switch_type::MUX;
            case SwitchType::PASS_GATE:
                return uxsd::enum_switch_type::PASS_GATE;
            case SwitchType::SHORT:
                return uxsd::enum_switch_type::SHORT;
            case SwitchType::BUFFER:
                return uxsd::enum_switch_type::BUFFER;
            default:
                report_error(
                    "Invalid switch type '%d'\n",
                    type);
        }

        return uxsd::enum_switch_type::UXSD_INVALID;
    }

    e_pin_type from_uxsd_pin_type(uxsd::enum_pin_type type) {
        switch (type) {
            case uxsd::enum_pin_type::OPEN:
                return OPEN;
            case uxsd::enum_pin_type::OUTPUT:
                return DRIVER;
            case uxsd::enum_pin_type::INPUT:
                return RECEIVER;
            default:
                report_error(
                    "Unknown pin class type %d", type);
        }
    }

    uxsd::enum_pin_type to_uxsd_pin_type(e_pin_type type) {
        switch (type) {
            case OPEN:
                return uxsd::enum_pin_type::OPEN;
            case DRIVER:
                return uxsd::enum_pin_type::OUTPUT;
            case RECEIVER:
                return uxsd::enum_pin_type::INPUT;
            default:
                report_error(
                    "Unknown pin class type %d", type);
        }
        return uxsd::enum_pin_type::UXSD_INVALID;
    }

    [[noreturn]] void report_error(const char* fmt, ...) {
        // Make a variable argument list
        va_list va_args;

        // Initialize variable argument list
        va_start(va_args, fmt);

        //Format string
        std::string str = vtr::vstring_fmt(fmt, va_args);

        // Reset variable argument list
        va_end(va_args);

        if (report_error_) {
            (*report_error_)(str.c_str());
        } else {
            throw std::runtime_error(str);
        }

        throw std::runtime_error("Unreachable line!");
    }

    // Temporary storage
    vtr::vector<RRIndexedDataId, short> seg_index_;
    std::string temp_string_;
    std::unordered_set<int> clock_net_virtual_sinks; // Temporary set storing the ID of nodes that have clk_res_type=virtual_sink

    // Constant mapping which is frequently used
    std::array<uxsd::enum_loc_side, 16> side_map_;

    // Output for loads, and constant data for writes.
    int* wire_to_rr_ipin_switch_;
    int* wire_to_rr_ipin_switch_between_dice_;
    t_chan_width* chan_width_;
    t_rr_graph_storage* rr_nodes_;
    RRGraphBuilder* rr_graph_builder_;
    RRGraphView* rr_graph_;
    vtr::vector<RRSwitchId, t_rr_switch_inf>* rr_switch_inf_;
    vtr::vector<RRIndexedDataId, t_rr_indexed_data>* rr_indexed_data_;
    t_rr_node_indices* rr_node_indices_;
    std::string* loaded_rr_graph_filename_;
    std::vector<t_rr_rc_data>* rr_rc_data_;

    // Constant data for loads and writes.
    const e_graph_type graph_type_;
    const enum e_base_cost_type base_cost_type_;
    const bool do_check_rr_graph_;
    const char* read_rr_graph_name_;
    const bool read_edge_metadata_;
    const bool echo_enabled_;
    const char* echo_file_name_;

    const std::vector<t_arch_switch_inf>& arch_switch_inf_;
    /*AA: The serializer does not support non-uniform Y & X directed channels yet. Will need to modify
     * the methods following routines:rr_graph_rr_nodes and init_node_segment according to the changes in 
     * rr_graph_indexed_data.cpp */
    const vtr::vector<RRSegmentId, t_segment_inf>& segment_inf_;
    std::vector<t_segment_inf> segment_inf_x_; // [num_segs_along_x_axis-1:0] - vector of segment information for segments along the x-axis.
    std::vector<t_segment_inf> segment_inf_y_; // [num_segs_along_y_axis-1:0] - vector of segment information for segments along the y-axis.
    const std::vector<t_physical_tile_type>& physical_tile_types_;
    const DeviceGrid& grid_;
    MetadataStorage<int>* rr_node_metadata_;
    MetadataStorage<std::tuple<int, int, short>>* rr_edge_metadata_;
    vtr::string_internment* strings_;
    vtr::interned_string empty_;
    const std::function<void(const char*)>* report_error_;
    bool is_flat_;

    // Temporary data to check grid block types
    int curr_tmp_block_type_id;
    int curr_tmp_height_offset;
    int curr_tmp_width_offset;
    int curr_tmp_layer;
    int curr_tmp_x;
    int curr_tmp_y;
};
