/** @file Utility fns for top-level router. */

#include "route_utils.h"

#include "connection_based_routing.h"
#include "net_delay.h"
#include "netlist_fwd.h"
#include "overuse_report.h"
#include "physical_types_util.h"
#include "place_and_route.h"
#include "route_common.h"
#include "route_debug.h"
#include "stats.h"

#include "VprTimingGraphResolver.h"
#include "route_tree.h"
#include "rr_graph.h"
#include "tatum/TimingReporter.hpp"
#include "stats.h"
#include "timing_util.h"

#ifdef VPR_USE_TBB
#include <tbb/combinable.h>
#include <tbb/parallel_for_each.h>
#endif // VPR_USE_TBB

#ifndef NO_GRAPHICS
#include "draw.h"
#include "draw_debug.h"
#include "draw_global.h"
#include "draw_types.h"
#include "vtr_expr_eval.h"
#endif // NO_GRAPHICS

bool check_net_delays(const Netlist<>& net_list, NetPinsMatrix<float>& net_delay) {
    constexpr float ERROR_TOL = 0.0001;

    auto net_delay_check = make_net_pins_matrix<float>(net_list);

    load_net_delay_from_routing(net_list, net_delay_check);

    for (ParentNetId net_id : net_list.nets()) {
        for (size_t ipin = 1; ipin < net_list.net_pins(net_id).size(); ipin++) {
            if (net_delay_check[net_id][ipin] == 0.) { /* Should be only GLOBAL nets */
                if (fabs(net_delay[net_id][ipin]) > ERROR_TOL) {
                    VPR_ERROR(VPR_ERROR_ROUTE,
                              "in timing_driven_check_net_delays: net %lu pin %d.\n"
                              "\tIncremental calc. net_delay is %g, but from scratch net delay is %g.\n",
                              size_t(net_id), ipin, net_delay[net_id][ipin], net_delay_check[net_id][ipin]);
                }
            } else {
                float error = fabs(1.0 - net_delay[net_id][ipin] / net_delay_check[net_id][ipin]);
                if (error > ERROR_TOL) {
                    VPR_ERROR(VPR_ERROR_ROUTE,
                              "in timing_driven_check_net_delays: net %d pin %lu.\n"
                              "\tIncremental calc. net_delay is %g, but from scratch net delay is %g.\n",
                              size_t(net_id), ipin, net_delay[net_id][ipin], net_delay_check[net_id][ipin]);
                }
            }
        }
    }

    return true;
}

// In heavily congested designs a static bounding box (BB) can
// become problematic for routability (it effectively enforces a
// hard blockage restricting where a net can route).
//
// For instance, the router will try to route non-critical connections
// away from congested regions, but may end up hitting the edge of the
// bounding box. Limiting how far out-of-the-way it can be routed, and
// preventing congestion from resolving.
//
// To alleviate this, we dynamically expand net bounding boxes if the net's
// *current* routing uses RR nodes 'close' to the edge of it's bounding box.
//
// The result is that connections trying to move out of the way and hitting
// their BB will have their bounding boxes will expand slowly in that direction.
// This helps spread out regions of heavy congestion (over several routing
// iterations).
//
// By growing the BBs slowly and only as needed we minimize the size of the BBs.
// This helps keep the router's graph search fast.
//
// Typically, only a small minority of nets (typically > 10%) have their BBs updated
// each routing iteration.
void dynamic_update_bounding_boxes(const std::vector<ParentNetId>& rerouted_nets,
                                   std::vector<ParentNetId> out_bb_updated_nets) {
    auto& device_ctx = g_vpr_ctx.device();
    auto& route_ctx = g_vpr_ctx.mutable_routing();

    auto& grid = device_ctx.grid;

    //Controls how close a net's routing needs to be to it's bounding box
    //before the bounding box is expanded.
    //
    //A value of zero indicates that the routing needs to be at the bounding box
    //edge
    constexpr int DYNAMIC_BB_DELTA_THRESHOLD = 0;

    //Walk through each net, calculating the bounding box of its current routing,
    //and then increase the router's bounding box if the two are close together

    int grid_xmax = grid.width() - 1;
    int grid_ymax = grid.height() - 1;

    for (ParentNetId net : rerouted_nets) {
        if (!route_ctx.route_trees[net])
            continue; // Skip if no routing
        if (!route_ctx.net_status.is_routed(net))
            continue;

        t_bb curr_bb = calc_current_bb(route_ctx.route_trees[net].value());
        t_bb& router_bb = route_ctx.route_bb[net];

        //Calculate the distances between the net's used RR nodes and
        //the router's bounding box
        int delta_xmin = curr_bb.xmin - router_bb.xmin;
        int delta_xmax = router_bb.xmax - curr_bb.xmax;
        int delta_ymin = curr_bb.ymin - router_bb.ymin;
        int delta_ymax = router_bb.ymax - curr_bb.ymax;

        //Note that if the net uses non-configurable switches it's routing
        //may end-up outside the bounding boxes, so the delta values may be
        //negative. The code below will expand the bounding box in those
        //cases.

        //Expand each dimension by one if within DYNAMIC_BB_DELTA_THRESHOLD threshold
        bool updated_bb = false;
        if (delta_xmin <= DYNAMIC_BB_DELTA_THRESHOLD && router_bb.xmin > 0) {
            --router_bb.xmin;
            updated_bb = true;
        }

        if (delta_ymin <= DYNAMIC_BB_DELTA_THRESHOLD && router_bb.ymin > 0) {
            --router_bb.ymin;
            updated_bb = true;
        }

        if (delta_xmax <= DYNAMIC_BB_DELTA_THRESHOLD && router_bb.xmax < grid_xmax) {
            ++router_bb.xmax;
            updated_bb = true;
        }

        if (delta_ymax <= DYNAMIC_BB_DELTA_THRESHOLD && router_bb.ymax < grid_ymax) {
            ++router_bb.ymax;
            updated_bb = true;
        }

        if (updated_bb) {
            out_bb_updated_nets.push_back(net);
            //VTR_LOG("Expanded net %6zu router BB to (%d,%d)x(%d,%d) based on net RR node BB (%d,%d)x(%d,%d)\n", size_t(net),
            //router_bb.xmin, router_bb.ymin, router_bb.xmax, router_bb.ymax,
            //curr_bb.xmin, curr_bb.ymin, curr_bb.xmax, curr_bb.ymax);
        }
    }
}

bool early_reconvergence_exit_heuristic(const t_router_opts& router_opts,
                                        int itry_since_last_convergence,
                                        std::shared_ptr<const SetupHoldTimingInfo> timing_info,
                                        const RoutingMetrics& best_routing_metrics) {
    if (itry_since_last_convergence == 1) {
        float cpd_ratio = timing_info->setup_worst_negative_slack() / best_routing_metrics.sWNS;

        // Give up if we see less than a 1% CPD improvement,
        // after reducing pres_fac. Typically larger initial
        // improvements are needed to see an actual improvement
        // in final legal routing quality.
        if (cpd_ratio >= router_opts.reconvergence_cpd_threshold) {
            VTR_LOG("Giving up routing since additional routing convergences seem unlikely to improve quality (CPD ratio: %g)\n", cpd_ratio);
            return true; // Potential CPD improvement is small, don't spend run-time trying to improve it
        }
    }

    return false; // Don't give up
}

bool is_better_quality_routing(const vtr::vector<ParentNetId, vtr::optional<RouteTree>>& best_routing,
                               const RoutingMetrics& best_routing_metrics,
                               const WirelengthInfo& wirelength_info,
                               std::shared_ptr<const SetupHoldTimingInfo> timing_info) {
    if (best_routing.empty()) {
        return true; // First legal routing
    }

    // Rank first based on sWNS, followed by other timing metrics
    if (timing_info) {
        if (timing_info->setup_worst_negative_slack() > best_routing_metrics.sWNS) {
            return true;
        } else if (timing_info->setup_worst_negative_slack() < best_routing_metrics.sWNS) {
            return false;
        }

        if (timing_info->setup_total_negative_slack() > best_routing_metrics.sTNS) {
            return true;
        } else if (timing_info->setup_total_negative_slack() < best_routing_metrics.sTNS) {
            return false;
        }

        if (timing_info->hold_worst_negative_slack() > best_routing_metrics.hWNS) {
            return true;
        } else if (timing_info->hold_worst_negative_slack() > best_routing_metrics.hWNS) {
            return false;
        }

        if (timing_info->hold_total_negative_slack() > best_routing_metrics.hTNS) {
            return true;
        } else if (timing_info->hold_total_negative_slack() > best_routing_metrics.hTNS) {
            return false;
        }
    }

    // Finally, wirelength tie breaker
    return wirelength_info.used_wirelength() < best_routing_metrics.used_wirelength;
}

bool is_iteration_complete(bool routing_is_feasible, const t_router_opts& router_opts, int itry, std::shared_ptr<const SetupHoldTimingInfo> timing_info, bool rcv_finished) {
    if (routing_is_feasible) {
        if (router_opts.routing_budgets_algorithm != YOYO) {
            return true;
        } else if (router_opts.routing_budgets_algorithm == YOYO && (timing_info->hold_worst_negative_slack() == 0 || rcv_finished) && itry != 1) {
            return true;
        }
    }
    return false;
}

void generate_route_timing_reports(const t_router_opts& router_opts,
                                   const t_analysis_opts& analysis_opts,
                                   const SetupTimingInfo& timing_info,
                                   const RoutingDelayCalculator& delay_calc,
                                   bool is_flat) {
    auto& timing_ctx = g_vpr_ctx.timing();
    auto& atom_ctx = g_vpr_ctx.atom();
    const auto& blk_loc_registry = g_vpr_ctx.placement().blk_loc_registry();
    const LogicalModels& models = g_vpr_ctx.device().arch->models;

    VprTimingGraphResolver resolver(atom_ctx.netlist(), atom_ctx.lookup(), models, *timing_ctx.graph, delay_calc, is_flat, blk_loc_registry);
    resolver.set_detail_level(analysis_opts.timing_report_detail);

    tatum::TimingReporter timing_reporter(resolver, *timing_ctx.graph, *timing_ctx.constraints);

    timing_reporter.report_timing_setup(router_opts.first_iteration_timing_report_file, *timing_info.setup_analyzer(), analysis_opts.timing_report_npaths);
}

int get_max_pins_per_net(const Netlist<>& net_list) {
    int max_pins_per_net = 0;
    for (auto net_id : net_list.nets()) {
        if (!net_list.net_is_ignored(net_id))
            max_pins_per_net = std::max(max_pins_per_net, (int)net_list.net_pins(net_id).size());
    }

    return (max_pins_per_net);
}

void print_overused_nodes_status(const t_router_opts& router_opts, const OveruseInfo& overuse_info) {
    VTR_LOG("\nFailed routing attempt\n");

    size_t num_overused = overuse_info.overused_nodes;
    size_t max_logged_overused_rr_nodes = router_opts.max_logged_overused_rr_nodes;

    //Overused nodes info logging upper limit
    VTR_LOG("Total number of overused nodes: %d\n", num_overused);
    if (num_overused > max_logged_overused_rr_nodes) {
        VTR_LOG("Total number of overused nodes is larger than the logging limit (%d).\n", max_logged_overused_rr_nodes);
        VTR_LOG("Displaying the first %d entries.\n", max_logged_overused_rr_nodes);
    }

    log_overused_nodes_status(max_logged_overused_rr_nodes);
    VTR_LOG("\n");
}

void print_route_status(int itry,
                        double elapsed_sec,
                        float pres_fac,
                        int num_bb_updated,
                        const RouterStats& router_stats,
                        const OveruseInfo& overuse_info,
                        const WirelengthInfo& wirelength_info,
                        std::shared_ptr<const SetupHoldTimingInfo> timing_info,
                        float est_success_iteration) {
    //Iteration
    VTR_LOG("%4d", itry);

    //Elapsed Time
    VTR_LOG(" %6.1f", elapsed_sec);

    //pres_fac
    constexpr int PRES_FAC_DIGITS = 7;
    constexpr int PRES_FAC_SCI_PRECISION = 1;
    pretty_print_float(" ", pres_fac, PRES_FAC_DIGITS, PRES_FAC_SCI_PRECISION);
    //VTR_LOG(" %5.1f", pres_fac);

    //Number of bounding boxes updated
    VTR_LOG(" %4d", num_bb_updated);

    //Heap push/pop
    constexpr int HEAP_OP_DIGITS = 7;
    constexpr int HEAP_OP_SCI_PRECISION = 2;
    pretty_print_uint(" ", router_stats.heap_pushes, HEAP_OP_DIGITS, HEAP_OP_SCI_PRECISION);
    VTR_ASSERT(router_stats.heap_pops <= router_stats.heap_pushes);

    //Rerouted nets
    constexpr int NET_ROUTED_DIGITS = 7;
    constexpr int NET_ROUTED_SCI_PRECISION = 2;
    pretty_print_uint(" ", router_stats.nets_routed, NET_ROUTED_DIGITS, NET_ROUTED_SCI_PRECISION);

    //Rerouted connections
    constexpr int CONN_ROUTED_DIGITS = 7;
    constexpr int CONN_ROUTED_SCI_PRECISION = 2;
    pretty_print_uint(" ", router_stats.connections_routed, CONN_ROUTED_DIGITS, CONN_ROUTED_SCI_PRECISION);

    //Overused RR nodes
    constexpr int OVERUSE_DIGITS = 7;
    constexpr int OVERUSE_SCI_PRECISION = 2;
    pretty_print_uint(" ", overuse_info.overused_nodes, OVERUSE_DIGITS, OVERUSE_SCI_PRECISION);
    VTR_LOG(" (%6.3f%%)", overuse_info.overused_node_ratio() * 100);

    //Wirelength
    constexpr int WL_DIGITS = 7;
    constexpr int WL_SCI_PRECISION = 2;
    pretty_print_uint(" ", wirelength_info.used_wirelength(), WL_DIGITS, WL_SCI_PRECISION);
    VTR_LOG(" (%4.1f%%)", wirelength_info.used_wirelength_ratio() * 100);

    //CPD
    if (timing_info) {
        float cpd = timing_info->least_slack_critical_path().delay();
        VTR_LOG(" %#8.3f", 1e9 * cpd);
    } else {
        VTR_LOG(" %8s", "N/A");
    }

    //sTNS
    if (timing_info) {
        float sTNS = timing_info->setup_total_negative_slack();
        VTR_LOG(" % #10.4g", 1e9 * sTNS);
    } else {
        VTR_LOG(" %10s", "N/A");
    }

    //sWNS
    if (timing_info) {
        float sWNS = timing_info->setup_worst_negative_slack();
        VTR_LOG(" % #10.3f", 1e9 * sWNS);
    } else {
        VTR_LOG(" %10s", "N/A");
    }

    //hTNS
    if (timing_info) {
        float hTNS = timing_info->hold_total_negative_slack();
        VTR_LOG(" % #10.4g", 1e9 * hTNS);
    } else {
        VTR_LOG(" %10s", "N/A");
    }

    //hWNS
    if (timing_info) {
        float hWNS = timing_info->hold_worst_negative_slack();
        VTR_LOG(" % #10.3f", 1e9 * hWNS);
    } else {
        VTR_LOG(" %10s", "N/A");
    }

    //Estimated success iteration
    if (std::isnan(est_success_iteration)) {
        VTR_LOG(" %8s", "N/A");
    } else {
        VTR_LOG(" %8.0f", est_success_iteration);
    }

    VTR_LOG("\n");

    fflush(stdout);
}

void print_route_status_header() {
    VTR_LOG("---- ------ ------- ---- ------- ------- ------- ----------------- --------------- -------- ---------- ---------- ---------- ---------- --------\n");
    VTR_LOG("Iter   Time    pres  BBs    Heap  Re-Rtd  Re-Rtd Overused RR Nodes      Wirelength      CPD       sTNS       sWNS       hTNS       hWNS Est Succ\n");
    VTR_LOG("      (sec)     fac Updt    push    Nets   Conns                                       (ns)       (ns)       (ns)       (ns)       (ns)     Iter\n");
    VTR_LOG("---- ------ ------- ---- ------- ------- ------- ----------------- --------------- -------- ---------- ---------- ---------- ---------- --------\n");
}

void print_router_criticality_histogram(const Netlist<>& net_list,
                                        const SetupTimingInfo& timing_info,
                                        const ClusteredPinAtomPinsLookup& netlist_pin_lookup,
                                        bool is_flat) {
    print_histogram(create_criticality_histogram(net_list, timing_info, netlist_pin_lookup, is_flat, 10));
}

void prune_unused_non_configurable_nets(CBRR& connections_inf,
                                        const Netlist<>& net_list) {
    auto& device_ctx = g_vpr_ctx.device();
    auto& route_ctx = g_vpr_ctx.mutable_routing();

    std::vector<int> non_config_node_set_usage(device_ctx.rr_non_config_node_sets.size(), 0);
    for (auto net_id : net_list.nets()) {
        if (!route_ctx.route_trees[net_id])
            continue;
        RouteTree& tree = route_ctx.route_trees[net_id].value();

        connections_inf.clear_force_reroute_for_net(net_id);

        std::vector<int> usage = tree.get_non_config_node_set_usage();

        // Prune the branches of the tree that don't legally lead to sinks
        tree.prune(connections_inf, &usage);
    }
}

vtr::vector<ParentNetId, std::vector<std::unordered_map<RRNodeId, int>>> set_nets_choking_spots(const Netlist<>& net_list,
                                                                                                const vtr::vector<ParentNetId,
                                                                                                                  std::vector<std::vector<int>>>& net_terminal_groups,
                                                                                                const vtr::vector<ParentNetId,
                                                                                                                  std::vector<int>>& net_terminal_group_num,
                                                                                                bool router_opt_choke_points,
                                                                                                bool is_flat) {
    vtr::vector<ParentNetId, std::vector<std::unordered_map<RRNodeId, int>>> choking_spots(net_list.nets().size());
    for (const auto& net_id : net_list.nets()) {
        choking_spots[net_id].resize(net_list.net_pins(net_id).size());
    }

    // Return if the architecture doesn't have any potential choke points or flat router is not enabled
    if (!router_opt_choke_points || !is_flat) {
        return choking_spots;
    }

    const auto& device_ctx = g_vpr_ctx.device();
    const auto& rr_graph = device_ctx.rr_graph;
    const auto& route_ctx = g_vpr_ctx.routing();
    const auto& net_rr_terminal = route_ctx.net_rr_terminals;

    for (const auto& net_id : net_list.nets()) {
        int pin_count = 0;
        // Global nets are not routed, thus we don't consider them.
        if (net_list.net_is_global(net_id)) {
            continue;
        }
        for (auto pin_id : net_list.net_pins(net_id)) {
            // pin_count == 0 corresponds to the net's source pin
            if (pin_count == 0) {
                pin_count++;
                continue;
            }
            auto block_id = net_list.pin_block(pin_id);
            auto blk_loc = get_block_loc(block_id, is_flat);
            int group_num = net_terminal_group_num[net_id][pin_count];
            // This is a group of sinks, including the current pin_id, which share a specific number of parent blocks.
            // To determine the choke points of the current sink, pin_id, we only consider the sinks in this group for the
            // run-time purpose
            std::vector<int> sink_grp = net_terminal_groups[net_id][group_num];
            VTR_ASSERT((int)sink_grp.size() >= 1);
            if (sink_grp.size() == 1) {
                pin_count++;
                continue;
            } else {
                // get the ptc_number of the sinks in the group
                std::for_each(sink_grp.begin(), sink_grp.end(), [&rr_graph](int& sink_rr_num) {
                    sink_rr_num = rr_graph.node_ptc_num(RRNodeId(sink_rr_num));
                });
                auto physical_type = device_ctx.grid.get_physical_type({blk_loc.loc.x, blk_loc.loc.y, blk_loc.loc.layer});
                // Get the choke points of the sink corresponds to pin_count given the sink group
                auto sink_choking_spots = get_sink_choking_points(physical_type,
                                                                  rr_graph.node_ptc_num(RRNodeId(net_rr_terminal[net_id][pin_count])),
                                                                  sink_grp);
                // Store choke points rr_node_id and the number reachable sinks
                for (const auto& choking_spot : sink_choking_spots) {
                    int pin_physical_num = choking_spot.first;
                    int num_reachable_sinks = choking_spot.second;
                    auto pin_rr_node_id = get_pin_rr_node_id(rr_graph.node_lookup(),
                                                             physical_type,
                                                             blk_loc.loc.layer,
                                                             blk_loc.loc.x,
                                                             blk_loc.loc.y,
                                                             pin_physical_num);
                    if (pin_rr_node_id != RRNodeId::INVALID()) {
                        choking_spots[net_id][pin_count].insert(std::make_pair(pin_rr_node_id, num_reachable_sinks));
                    }
                }
            }
            pin_count++;
        }
    }

    return choking_spots;
}

/** Wrapper for create_rr_graph() with extra checks */
void try_graph(int width_fac,
               const t_router_opts& router_opts,
               t_det_routing_arch& det_routing_arch,
               std::vector<t_segment_inf>& segment_inf,
               t_chan_width_dist chan_width_dist,
               const std::vector<t_direct_inf>& directs,
               bool is_flat) {
    auto& device_ctx = g_vpr_ctx.mutable_device();

    e_graph_type graph_type;
    e_graph_type graph_directionality;
    if (router_opts.route_type == e_route_type::GLOBAL) {
        graph_type = e_graph_type::GLOBAL;
        graph_directionality = e_graph_type::BIDIR;
    } else {
        graph_type = (det_routing_arch.directionality == BI_DIRECTIONAL ? e_graph_type::BIDIR : e_graph_type::UNIDIR);
        graph_directionality = (det_routing_arch.directionality == BI_DIRECTIONAL ? e_graph_type::BIDIR : e_graph_type::UNIDIR);
    }

    /* Set the channel widths */
    t_chan_width chan_width = init_chan(width_fac, chan_width_dist, graph_directionality);

    /* Free any old routing graph, if one exists. */
    free_rr_graph();

    /* Set up the routing resource graph defined by this FPGA architecture. */
    int warning_count;
    create_rr_graph(graph_type,
                    device_ctx.physical_tile_types,
                    device_ctx.grid,
                    chan_width,
                    det_routing_arch,
                    segment_inf,
                    router_opts,
                    directs,
                    &warning_count,
                    is_flat);
}

bool early_exit_heuristic(const t_router_opts& router_opts, const WirelengthInfo& wirelength_info) {
    if (wirelength_info.used_wirelength_ratio() > router_opts.init_wirelength_abort_threshold) {
        VTR_LOG("Wire length usage ratio %g exceeds limit of %g, fail routing.\n",
                wirelength_info.used_wirelength_ratio(),
                router_opts.init_wirelength_abort_threshold);
        return true;
    }
    return false;
}

size_t calculate_wirelength_available() {
    auto& device_ctx = g_vpr_ctx.device();
    const auto& rr_graph = device_ctx.rr_graph;

    size_t available_wirelength = 0;
    // But really what's happening is that this for loop iterates over every node and determines the available wirelength
    for (RRNodeId rr_id : device_ctx.rr_graph.nodes()) {
        const e_rr_type channel_type = rr_graph.node_type(rr_id);
        if (channel_type == e_rr_type::CHANX || channel_type == e_rr_type::CHANY) {
            available_wirelength += rr_graph.node_capacity(rr_id) * rr_graph.node_length(rr_id);
        }
    }
    return available_wirelength;
}

WirelengthInfo calculate_wirelength_info(const Netlist<>& net_list, size_t available_wirelength) {
    size_t used_wirelength = 0;
    VTR_ASSERT(available_wirelength > 0);

    auto& route_ctx = g_vpr_ctx.routing();

#ifdef VPR_USE_TBB
    tbb::combinable<size_t> thread_used_wirelength(0);

    tbb::parallel_for_each(net_list.nets().begin(), net_list.nets().end(), [&](ParentNetId net_id) {
        if (!net_list.net_is_ignored(net_id)
            && net_list.net_sinks(net_id).size() != 0 /* Globals don't count. */
            && route_ctx.route_trees[net_id]) {
            int bends, wirelength, segments;
            bool is_absorbed;
            get_num_bends_and_length(net_id, &bends, &wirelength, &segments, &is_absorbed);

            thread_used_wirelength.local() += wirelength;
        }
    });

    used_wirelength = thread_used_wirelength.combine(std::plus<size_t>());
#else
    for (auto net_id : net_list.nets()) {
        if (!net_list.net_is_ignored(net_id)
            && net_list.net_sinks(net_id).size() != 0 /* Globals don't count. */
            && route_ctx.route_trees[net_id]) {
            int bends = 0, wirelength = 0, segments = 0;
            bool is_absorbed;
            get_num_bends_and_length(net_id, &bends, &wirelength, &segments, &is_absorbed);

            used_wirelength += wirelength;
        }
    }
#endif

    return WirelengthInfo(available_wirelength, used_wirelength);
}

t_bb calc_current_bb(const RouteTree& tree) {
    auto& device_ctx = g_vpr_ctx.device();
    const auto& rr_graph = device_ctx.rr_graph;
    auto& grid = device_ctx.grid;

    t_bb bb;
    bb.xmin = grid.width() - 1;
    bb.ymin = grid.height() - 1;
    bb.layer_min = grid.get_num_layers() - 1;
    bb.xmax = 0;
    bb.ymax = 0;
    bb.layer_max = 0;

    for (const RouteTreeNode& rt_node : tree.all_nodes()) {
        //The router interprets RR nodes which cross the boundary as being
        //'within' of the BB. Only those which are *strictly* out side the
        //box are excluded, hence we use the nodes xhigh/yhigh for xmin/xmax,
        //and xlow/ylow for xmax/ymax calculations
        bb.xmin = std::min<int>(bb.xmin, rr_graph.node_xhigh(rt_node.inode));
        bb.ymin = std::min<int>(bb.ymin, rr_graph.node_yhigh(rt_node.inode));
        bb.layer_min = std::min<int>(bb.layer_min, rr_graph.node_layer(rt_node.inode));
        bb.xmax = std::max<int>(bb.xmax, rr_graph.node_xlow(rt_node.inode));
        bb.ymax = std::max<int>(bb.ymax, rr_graph.node_ylow(rt_node.inode));
        bb.layer_max = std::max<int>(bb.layer_max, rr_graph.node_layer(rt_node.inode));
    }

    VTR_ASSERT(bb.xmin <= bb.xmax);
    VTR_ASSERT(bb.ymin <= bb.ymax);

    return bb;
}

// Initializes net_delay based on best-case delay estimates from the router lookahead
void init_net_delay_from_lookahead(const RouterLookahead& router_lookahead,
                                   const Netlist<>& net_list,
                                   const vtr::vector<ParentNetId, std::vector<RRNodeId>>& net_rr_terminals,
                                   NetPinsMatrix<float>& net_delay,
                                   const RRGraphView& rr_graph,
                                   bool is_flat) {
    t_conn_cost_params cost_params;
    cost_params.criticality = 1.; // Ensures lookahead returns delay value

    for (ParentNetId net_id : net_list.nets()) {
        if (net_list.net_is_ignored(net_id)) continue;

        RRNodeId source_rr = net_rr_terminals[net_id][0];

        for (size_t ipin = 1; ipin < net_list.net_pins(net_id).size(); ++ipin) {
            RRNodeId sink_rr = net_rr_terminals[net_id][ipin];

            float est_delay = get_cost_from_lookahead(router_lookahead,
                                                      rr_graph,
                                                      source_rr,
                                                      sink_rr,
                                                      0.0f /* R_upstream */,
                                                      cost_params,
                                                      is_flat);
            VTR_ASSERT(std::isfinite(est_delay) && est_delay < std::numeric_limits<float>::max());

            net_delay[net_id][ipin] = est_delay;
        }
    }
}

#ifndef NO_GRAPHICS
void update_draw_pres_fac(const float new_pres_fac) {
#else
void update_draw_pres_fac(const float /*new_pres_fac*/) {
#endif
#ifndef NO_GRAPHICS
    // Only updates the drawing pres_fac if graphics is enabled
    get_draw_state_vars()->pres_fac = new_pres_fac;

#endif // NO_GRAPHICS
}

#ifndef NO_GRAPHICS
void update_router_info_and_check_bp(bp_router_type type, int net_id) {
    t_draw_state* draw_state = get_draw_state_vars();
    if (!draw_state->list_of_breakpoints.empty()) {
        if (type == BP_ROUTE_ITER)
            get_bp_state_globals()->get_glob_breakpoint_state()->router_iter++;
        else if (type == BP_NET_ID)
            get_bp_state_globals()->get_glob_breakpoint_state()->route_net_id = net_id;
        f_router_debug = check_for_breakpoints(false);
        if (f_router_debug) {
            breakpoint_info_window(get_bp_state_globals()->get_glob_breakpoint_state()->bp_description, *get_bp_state_globals()->get_glob_breakpoint_state(), false);
            update_screen(ScreenUpdatePriority::MAJOR, "Breakpoint Encountered", ROUTING, nullptr);
        }
    }
}
#endif

bool is_net_routed(ParentNetId net_id) {
    const auto& route_ctx = g_vpr_ctx.routing();
    //Note: we can't use route_ctx.net_status.is_routed(atom_net_id), because net_status is filled only when route stage took place
    return route_ctx.route_trees[net_id].has_value();
}

bool is_net_fully_absorbed(ParentNetId net_id) {
    const RRGraphView& rr_graph = g_vpr_ctx.device().rr_graph;
    const RoutingContext& route_ctx = g_vpr_ctx.routing();

    bool is_absorbed = true;

    for (auto& rt_node : route_ctx.route_trees[net_id].value().all_nodes()) {
        RRNodeId inode = rt_node.inode;

        e_rr_type rr_type = rr_graph.node_type(inode);

        if (rr_type == e_rr_type::CHANX || rr_type == e_rr_type::CHANY) {
            is_absorbed = false;
            break;
        }
    }

    return is_absorbed;
}
