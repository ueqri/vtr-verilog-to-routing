#pragma once
/**
 * @file intra_logic_block.h
 * 
 * This file manages the interactions between logic blocks, cluster blocks, and their sub blocks
 * Contains declaration of selected_Sub_block_info struct, which holds the information on
 * the currently selected/highlighted block
 * 
 * Authors: Long Yu Wang, Matthew J.P. Walker, Sebastian Lievano
 */

/* Author: Long Yu Wang
 * Date: August 2013
 *
 * Author: Matthew J.P. Walker
 * Date: May,June 2014
 */

#ifndef NO_GRAPHICS

#include "vpr_types.h"
#include "atom_netlist_fwd.h"
#include <unordered_set>

#include "ezgl/point.hpp"
#include "ezgl/graphics.hpp"

struct t_selected_sub_block_info {
    struct clb_pin_tuple {
        ClusterBlockId clb_index;
        const t_pb_graph_node* pb_gnode;

        clb_pin_tuple(ClusterBlockId clb_index, const t_pb_graph_node* pb_gnode);
        clb_pin_tuple(const AtomPinId atom_pin);
        bool operator==(const clb_pin_tuple&) const;
    };

    struct gnode_clb_pair {
        const t_pb_graph_node* pb_gnode = nullptr;
        const ClusterBlockId clb_index;
        gnode_clb_pair() = default;
        gnode_clb_pair(const t_pb_graph_node* pb_gnode, const ClusterBlockId clb_index);
        bool operator==(const gnode_clb_pair&) const;
    };

    struct sel_subblk_hasher {
        inline std::size_t operator()(const gnode_clb_pair& v) const {
            std::hash<const void*> ptr_hasher;
            return ptr_hasher((const void*)v.pb_gnode) ^ ptr_hasher((const void*)&v.clb_index);
        }

        inline std::size_t operator()(const std::pair<clb_pin_tuple, clb_pin_tuple>& v) const {
            return (*this)(v.first) ^ (*this)(v.second);
        }

        inline std::size_t operator()(const clb_pin_tuple& v) const {
            std::hash<int> int_hasher;
            std::hash<const void*> ptr_hasher;
            return int_hasher(size_t(v.clb_index))
                   ^ ptr_hasher((const void*)v.pb_gnode);
        }
    };

  private:
    t_pb* selected_pb;
    ClusterBlockId containing_block_index;
    t_pb_graph_node* selected_pb_gnode;
    std::unordered_set<gnode_clb_pair, sel_subblk_hasher> sinks;
    std::unordered_set<gnode_clb_pair, sel_subblk_hasher> sources;
    std::unordered_set<gnode_clb_pair, sel_subblk_hasher> in_selected_subtree;

  public:
    t_selected_sub_block_info();

    void set(t_pb* new_selected_sub_block, const ClusterBlockId containing_block_index);
    t_pb_graph_node* get_selected_pb_gnode() const;
    ClusterBlockId get_containing_block() const;

    /*
     * gets the t_pb that is currently selected. Please don't use this if
     * you think you can get away with using is_selected(...) or get_selected_pb_gnode() and
     * get_containing_block(). May disappear in future.
     */
    t_pb* get_selected_pb() const;

    bool has_selection() const;
    void clear();
    // pb related selection test functions
    bool is_selected(const t_pb_graph_node* test, const ClusterBlockId clb_index) const;
    bool is_sink_of_selected(const t_pb_graph_node* test, const ClusterBlockId clb_index) const;
    bool is_source_of_selected(const t_pb_graph_node* test, const ClusterBlockId clb_index) const;
    bool is_in_selected_subtree(const t_pb_graph_node* test, const ClusterBlockId clb_index) const;
};

/* This function pre-allocates space to store bounding boxes for all sub-blocks. Each
 * sub-block is identified by its descriptor_type and a unique pin ID in the type.
 */
void draw_internal_alloc_blk();

/* This function initializes bounding box values for all sub-blocks. It calls helper functions
 * to traverse through the pb_graph for each descriptor_type and compute bounding box values.
 */
void draw_internal_init_blk();

/* Top-level drawing routine for internal sub-blocks. The function traverses through all
 * grid tiles and calls helper function to draw inside each block.
 */
void draw_internal_draw_subblk(ezgl::renderer* g);

/* Determines which part of a block to highlight, and stores it,
 * so that the other subblock drawing functions will obey it.
 * If the user missed all sub-parts, will return 1, else 0.
 */
int highlight_sub_block(const ezgl::point2d& point_in_clb, const ClusterBlockId clb_index, t_pb* pb);

/*
 * returns the struct with information about the sub-block selection
 */
t_selected_sub_block_info& get_selected_sub_block_info();

/*
 * Draws lines from the proper logical sources, to the proper logical sinks.
 * If the draw state says to show all logical connections, it will,
 * and if there is a selected sub-block, it will highlight it's conections
 */

void draw_logical_connections(ezgl::renderer* g);

void find_pin_index_at_model_scope(const AtomPinId the_pin, const AtomBlockId lblk, int* pin_index, int* total_pins);

//Returns pb ptr of given atom block name, given the pb of its containing block. Returns null if nothing found
t_pb* find_atom_block_in_pb(const std::string& name, t_pb* pb);

#endif /* NO_GRAPHICS */
