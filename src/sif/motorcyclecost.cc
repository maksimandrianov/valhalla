#include "sif/motorcyclecost.h"
#include "baldr/accessrestriction.h"
#include "baldr/directededge.h"
#include "baldr/graphconstants.h"
#include "baldr/nodeinfo.h"
#include "midgard/constants.h"
#include "midgard/util.h"
#include "proto_conversions.h"
#include "sif/costconstants.h"
#include "sif/osrm_car_duration.h"
#include <cassert>

#ifdef INLINE_TEST
#include "test.h"
#include "worker.h"
#include <random>
#endif

using namespace valhalla::midgard;
using namespace valhalla::baldr;

namespace valhalla {
namespace sif {

// Default options/values
namespace {

// Base transition costs
// TODO - can we define these in dynamiccost.h and override here if they differ?
constexpr float kDefaultDestinationOnlyPenalty = 600.0f; // Seconds
constexpr float kDefaultManeuverPenalty = 5.0f;          // Seconds
constexpr float kDefaultAlleyPenalty = 5.0f;             // Seconds
constexpr float kDefaultGateCost = 30.0f;                // Seconds
constexpr float kDefaultGatePenalty = 300.0f;            // Seconds
constexpr float kDefaultTollBoothCost = 15.0f;           // Seconds
constexpr float kDefaultTollBoothPenalty = 0.0f;         // Seconds
constexpr float kDefaultFerryCost = 300.0f;              // Seconds
constexpr float kDefaultCountryCrossingCost = 600.0f;    // Seconds
constexpr float kDefaultCountryCrossingPenalty = 0.0f;   // Seconds
constexpr float kDefaultServicePenalty = 15.0f;          // Seconds

// Other options
constexpr float kDefaultUseFerry = 0.5f;         // Factor between 0 and 1
constexpr float kDefaultUseHighways = 0.5f;      // Factor between 0 and 1
constexpr float kDefaultUseTolls = 0.5f;         // Factor between 0 and 1
constexpr float kDefaultUseTrails = 0.0f;        // Factor between 0 and 1
constexpr float kDefaultUseTracks = 0.5f;        // Factor between 0 and 1
constexpr float kDefaultUseLivingStreets = 0.1f; // Factor between 0 and 1

constexpr Surface kMinimumMotorcycleSurface = Surface::kDirt;

// Default turn costs
constexpr float kTCStraight = 0.5f;
constexpr float kTCSlight = 0.75f;
constexpr float kTCFavorable = 1.0f;
constexpr float kTCFavorableSharp = 1.5f;
constexpr float kTCCrossing = 2.0f;
constexpr float kTCUnfavorable = 2.5f;
constexpr float kTCUnfavorableSharp = 3.5f;
constexpr float kTCReverse = 9.5f;

// Turn costs based on side of street driving
constexpr float kRightSideTurnCosts[] = {kTCStraight,       kTCSlight,  kTCFavorable,
                                         kTCFavorableSharp, kTCReverse, kTCUnfavorableSharp,
                                         kTCUnfavorable,    kTCSlight};
constexpr float kLeftSideTurnCosts[] = {kTCStraight,         kTCSlight,  kTCUnfavorable,
                                        kTCUnfavorableSharp, kTCReverse, kTCFavorableSharp,
                                        kTCFavorable,        kTCSlight};

// How much to avoid generic service roads.
constexpr float kDefaultServiceFactor = 1.0f;

constexpr float kMinFactor = 0.1f;
constexpr float kMaxFactor = 100000.0f;

// Valid ranges and defaults
constexpr ranged_default_t<float> kManeuverPenaltyRange{0, kDefaultManeuverPenalty, kMaxPenalty};
constexpr ranged_default_t<float> kAlleyPenaltyRange{0, kDefaultAlleyPenalty, kMaxPenalty};
constexpr ranged_default_t<float> kGateCostRange{0, kDefaultGateCost, kMaxPenalty};
constexpr ranged_default_t<float> kGatePenaltyRange{0, kDefaultGatePenalty, kMaxPenalty};
constexpr ranged_default_t<float> kTollBoothCostRange{0, kDefaultTollBoothCost, kMaxPenalty};
constexpr ranged_default_t<float> kTollBoothPenaltyRange{0, kDefaultTollBoothPenalty, kMaxPenalty};
constexpr ranged_default_t<float> kFerryCostRange{0, kDefaultFerryCost, kMaxPenalty};
constexpr ranged_default_t<float> kCountryCrossingCostRange{0, kDefaultCountryCrossingCost,
                                                            kMaxPenalty};
constexpr ranged_default_t<float> kCountryCrossingPenaltyRange{0, kDefaultCountryCrossingPenalty,
                                                               kMaxPenalty};
constexpr ranged_default_t<float> kUseFerryRange{0, kDefaultUseFerry, 1.0f};
constexpr ranged_default_t<float> kUseHighwaysRange{0, kDefaultUseHighways, 1.0f};
constexpr ranged_default_t<float> kUseTollsRange{0, kDefaultUseTolls, 1.0f};
constexpr ranged_default_t<float> kUseTrailsRange{0, kDefaultUseTrails, 1.0f};
constexpr ranged_default_t<float> kDestinationOnlyPenaltyRange{0, kDefaultDestinationOnlyPenalty,
                                                               kMaxPenalty};
constexpr ranged_default_t<float> kUseTracksRange{0.f, kDefaultUseTracks, 1.0f};
constexpr ranged_default_t<float> kUseLivingStreetsRange{0.f, kDefaultUseLivingStreets, 1.0f};
constexpr ranged_default_t<float> kServicePenaltyRange{0.0f, kDefaultServicePenalty, kMaxPenalty};
constexpr ranged_default_t<float> kServiceFactorRange{kMinFactor, kDefaultServiceFactor, kMaxFactor};

// Maximum highway avoidance bias (modulates the highway factors based on road class)
constexpr float kMaxHighwayBiasFactor = 8.0f;

constexpr float kHighwayFactor[] = {
    1.0f, // Motorway
    0.5f, // Trunk
    0.0f, // Primary
    0.0f, // Secondary
    0.0f, // Tertiary
    0.0f, // Unclassified
    0.0f, // Residential
    0.0f  // Service, other
};

constexpr float kMaxTrailBiasFactor = 8.0f;

constexpr float kSurfaceFactor[] = {
    0.0f, // kPavedSmooth
    0.0f, // kPaved
    0.0f, // kPaveRough
    0.1f, // kCompacted
    0.2f, // kDirt
    0.5f, // kGravel
    1.0f  // kPath
};

} // namespace

/**
 * Derived class providing dynamic edge costing for "direct" auto routes. This
 * is a route that is generally shortest time but uses route hierarchies that
 * can result in slightly longer routes that avoid shortcuts on residential
 * roads.
 */
class MotorcycleCost : public DynamicCost {
public:
  /**
   * Construct motorcycle costing. Pass in cost type and costing_options using protocol buffer(pbf).
   * @param  costing specified costing type.
   * @param  costing_options pbf with request costing_options.
   */
  MotorcycleCost(const CostingOptions& costing_options);

  virtual ~MotorcycleCost();

  /**
   * Does the costing method allow multiple passes (with relaxed hierarchy
   * limits).
   * @return  Returns true if the costing model allows multiple passes.
   */
  virtual bool AllowMultiPass() const override {
    return true;
  }

  /**
   * Checks if access is allowed for the provided directed edge.
   * This is generally based on mode of travel and the access modes
   * allowed on the edge. However, it can be extended to exclude access
   * based on other parameters such as conditional restrictions and
   * conditional access that can depend on time and travel mode.
   * @param  edge           Pointer to a directed edge.
   * @param  pred           Predecessor edge information.
   * @param  tile           Current tile.
   * @param  edgeid         GraphId of the directed edge.
   * @param  current_time   Current time (seconds since epoch). A value of 0
   *                        indicates the route is not time dependent.
   * @param  tz_index       timezone index for the node
   * @return Returns true if access is allowed, false if not.
   */
  virtual bool Allowed(const baldr::DirectedEdge* edge,
                       const EdgeLabel& pred,
                       const graph_tile_ptr& tile,
                       const baldr::GraphId& edgeid,
                       const uint64_t current_time,
                       const uint32_t tz_index,
                       uint8_t& restriction_idx) const override;

  /**
   * Checks if access is allowed for an edge on the reverse path
   * (from destination towards origin). Both opposing edges (current and
   * predecessor) are provided. The access check is generally based on mode
   * of travel and the access modes allowed on the edge. However, it can be
   * extended to exclude access based on other parameters such as conditional
   * restrictions and conditional access that can depend on time and travel
   * mode.
   * @param  edge           Pointer to a directed edge.
   * @param  pred           Predecessor edge information.
   * @param  opp_edge       Pointer to the opposing directed edge.
   * @param  tile           Current tile.
   * @param  edgeid         GraphId of the opposing edge.
   * @param  current_time   Current time (seconds since epoch). A value of 0
   *                        indicates the route is not time dependent.
   * @param  tz_index       timezone index for the node
   * @return  Returns true if access is allowed, false if not.
   */
  virtual bool AllowedReverse(const baldr::DirectedEdge* edge,
                              const EdgeLabel& pred,
                              const baldr::DirectedEdge* opp_edge,
                              const graph_tile_ptr& tile,
                              const baldr::GraphId& opp_edgeid,
                              const uint64_t current_time,
                              const uint32_t tz_index,
                              uint8_t& restriction_idx) const override;

  /**
   * Only transit costings are valid for this method call, hence we throw
   * @param edge
   * @param departure
   * @param curr_time
   * @return
   */
  virtual Cost EdgeCost(const baldr::DirectedEdge*,
                        const baldr::TransitDeparture*,
                        const uint32_t) const override {
    throw std::runtime_error("MotorcycleCost::EdgeCost does not support transit edges");
  }

  /**
   * Get the cost to traverse the specified directed edge. Cost includes
   * the time (seconds) to traverse the edge.
   * @param  edge      Pointer to a directed edge.
   * @param  tile      Current tile.
   * @param  seconds   Time of week in seconds.
   * @return  Returns the cost and time (seconds)
   */
  virtual Cost EdgeCost(const baldr::DirectedEdge* edge,
                        const graph_tile_ptr& tile,
                        const uint32_t seconds,
                        uint8_t& flow_sources) const override;

  /**
   * Returns the cost to make the transition from the predecessor edge.
   * Defaults to 0. Costing models that wish to include edge transition
   * costs (i.e., intersection/turn costs) must override this method.
   * @param  edge  Directed edge (the to edge)
   * @param  node  Node (intersection) where transition occurs.
   * @param  pred  Predecessor edge information.
   * @return  Returns the cost and time (seconds)
   */
  virtual Cost TransitionCost(const baldr::DirectedEdge* edge,
                              const baldr::NodeInfo* node,
                              const EdgeLabel& pred) const override;

  /**
   * Returns the cost to make the transition from the predecessor edge
   * when using a reverse search (from destination towards the origin).
   * @param  idx   Directed edge local index
   * @param  node  Node (intersection) where transition occurs.
   * @param  pred  the opposing current edge in the reverse tree.
   * @param  edge  the opposing predecessor in the reverse tree
   * @param  has_measured_speed Do we have any of the measured speed types set?
   * @param  internal_turn  Did we make an turn on a short internal edge.
   * @return  Returns the cost and time (seconds)
   */
  virtual Cost TransitionCostReverse(const uint32_t idx,
                                     const baldr::NodeInfo* node,
                                     const baldr::DirectedEdge* pred,
                                     const baldr::DirectedEdge* edge,
                                     const bool has_measured_speed,
                                     const InternalTurn /*internal_turn*/) const override;

  /**
   * Get the cost factor for A* heuristics. This factor is multiplied
   * with the distance to the destination to produce an estimate of the
   * minimum cost to the destination. The A* heuristic must underestimate the
   * cost to the destination. So a time based estimate based on speed should
   * assume the maximum speed is used to the destination such that the time
   * estimate is less than the least possible time along roads.
   */
  virtual float AStarCostFactor() const override {
    return speedfactor_[top_speed_];
  }

  /**
   * Get the current travel type.
   * @return  Returns the current travel type.
   */
  virtual uint8_t travel_type() const override {
    return static_cast<uint8_t>(VehicleType::kMotorcycle);
  }

  /**
   * Function to be used in location searching which will
   * exclude and allow ranking results from the search by looking at each
   * edges attribution and suitability for use as a location by the travel
   * mode used by the costing method. It's also used to filter
   * edges not usable / inaccessible by automobile.
   */
  bool Allowed(const baldr::DirectedEdge* edge,
               const graph_tile_ptr& tile,
               uint16_t disallow_mask = kDisallowNone) const override {
    bool allow_closures = (!filter_closures_ && !(disallow_mask & kDisallowClosure)) ||
                          !(flow_mask_ & kCurrentFlowMask);
    return DynamicCost::Allowed(edge, tile, disallow_mask) && !edge->bss_connection() &&
           (allow_closures || !tile->IsClosed(edge));
  }
  // Hidden in source file so we don't need it to be protected
  // We expose it within the source file for testing purposes
public:
  VehicleType type_; // Vehicle type: car (default), motorcycle, etc
  std::vector<float> speedfactor_;
  float density_factor_[16]; // Density factor
  float ferry_factor_;       // Weighting to apply to ferry edges
  float toll_factor_;        // Factor applied when road has a toll
  float surface_factor_;     // How much the surface factors are applied when using trails
  float highway_factor_;     // Factor applied when road is a motorway or trunk

  // Density factor used in edge transition costing
  std::vector<float> trans_density_factor_;
};

// Constructor
MotorcycleCost::MotorcycleCost(const CostingOptions& costing_options)
    : DynamicCost(costing_options, TravelMode::kDrive, kMotorcycleAccess),
      trans_density_factor_{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.1f, 1.2f, 1.3f,
                            1.4f, 1.6f, 1.9f, 2.2f, 2.5f, 2.8f, 3.1f, 3.5f} {

  // Vehicle type is motorcycle
  type_ = VehicleType::kMotorcycle;

  // Get the base costs
  get_base_costs(costing_options);

  // Preference to use highways. Is a value from 0 to 1
  // Factor for highway use - use a non-linear factor with values at 0.5 being neutral (factor
  // of 0). Values between 0.5 and 1 slowly decrease to a maximum of -0.125 (to slightly prefer
  // highways) while values between 0.5 to 0 slowly increase to a maximum of kMaxHighwayBiasFactor
  // to avoid/penalize highways.
  float use_highways = costing_options.use_highways();
  if (use_highways >= 0.5f) {
    float f = (0.5f - use_highways);
    highway_factor_ = f * f * f;
  } else {
    float f = 1.0f - (use_highways * 2.0f);
    highway_factor_ = kMaxHighwayBiasFactor * (f * f);
  }

  // Set toll factor based on preference to use tolls (value from 0 to 1).
  // Toll factor of 0 would indicate no adjustment to weighting for toll roads.
  // use_tolls = 1 would reduce weighting slightly (a negative delta) while
  // use_tolls = 0 would penalize (positive delta to weighting factor).
  float use_tolls = costing_options.use_tolls();
  toll_factor_ = use_tolls < 0.5f ? (2.0f - 4 * use_tolls) : // ranges from 2 to 0
                     (0.5f - use_tolls) * 0.03f;             // ranges from 0 to -0.15

  // Set the surface factor based on the use trails value - this is a
  // preference to use trails/tracks/bad surface types (a value from 0 to 1).
  float use_trails = costing_options.use_trails();

  // Factor for trail use - use a non-linear factor with values at 0.5 being neutral (factor
  // of 0). Values between 0.5 and 1 slowly decrease to a maximum of -0.125 (to slightly prefer
  // trails) while values between 0.5 to 0 slowly increase to a maximum of the surfact_factor_
  // to avoid/penalize trails.
  // modulates surface factor based on use_trails
  if (use_trails >= 0.5f) {
    float f = (0.5f - use_trails);
    surface_factor_ = f * f * f;
  } else {
    float f = 1.0f - use_trails * 2.0f;
    surface_factor_ = static_cast<uint32_t>(kMaxTrailBiasFactor * (f * f));
  }

  // Create speed cost table
  speedfactor_.resize(kMaxSpeedKph + 1, 0);
  speedfactor_[0] = kSecPerHour; // TODO - what to make speed=0?
  for (uint32_t s = 1; s <= kMaxSpeedKph; s++) {
    speedfactor_[s] = (kSecPerHour * 0.001f) / static_cast<float>(s);
  }

  // Set density factors - used to penalize edges in dense, urban areas
  for (uint32_t d = 0; d < 16; d++) {
    density_factor_[d] = 0.85f + (d * 0.018f);
  }
}

// Destructor
MotorcycleCost::~MotorcycleCost() {
}

// Check if access is allowed on the specified edge.
bool MotorcycleCost::Allowed(const baldr::DirectedEdge* edge,
                             const EdgeLabel& pred,
                             const graph_tile_ptr& tile,
                             const baldr::GraphId& edgeid,
                             const uint64_t current_time,
                             const uint32_t tz_index,
                             uint8_t& restriction_idx) const {
  // Check access, U-turn, and simple turn restriction.
  // Allow U-turns at dead-end nodes.
  if (!IsAccessible(edge) || (!pred.deadend() && pred.opp_local_idx() == edge->localedgeidx()) ||
      ((pred.restrictions() & (1 << edge->localedgeidx())) && !ignore_restrictions_) ||
      (edge->surface() > kMinimumMotorcycleSurface) || IsUserAvoidEdge(edgeid) ||
      (!allow_destination_only_ && !pred.destonly() && edge->destonly()) ||
      (pred.closure_pruning() && IsClosed(edge, tile))) {
    return false;
  }

  return DynamicCost::EvaluateRestrictions(access_mask_, edge, tile, edgeid, current_time, tz_index,
                                           restriction_idx);
}

// Checks if access is allowed for an edge on the reverse path (from
// destination towards origin). Both opposing edges are provided.
bool MotorcycleCost::AllowedReverse(const baldr::DirectedEdge* edge,
                                    const EdgeLabel& pred,
                                    const baldr::DirectedEdge* opp_edge,
                                    const graph_tile_ptr& tile,
                                    const baldr::GraphId& opp_edgeid,
                                    const uint64_t current_time,
                                    const uint32_t tz_index,
                                    uint8_t& restriction_idx) const {
  // Check access, U-turn, and simple turn restriction.
  // Allow U-turns at dead-end nodes.
  if (!IsAccessible(opp_edge) || (!pred.deadend() && pred.opp_local_idx() == edge->localedgeidx()) ||
      ((opp_edge->restrictions() & (1 << pred.opp_local_idx())) && !ignore_restrictions_) ||
      (opp_edge->surface() > kMinimumMotorcycleSurface) || IsUserAvoidEdge(opp_edgeid) ||
      (!allow_destination_only_ && !pred.destonly() && opp_edge->destonly()) ||
      (pred.closure_pruning() && IsClosed(opp_edge, tile))) {
    return false;
  }

  return DynamicCost::EvaluateRestrictions(access_mask_, edge, tile, opp_edgeid, current_time,
                                           tz_index, restriction_idx);
}

Cost MotorcycleCost::EdgeCost(const baldr::DirectedEdge* edge,
                              const graph_tile_ptr& tile,
                              const uint32_t seconds,
                              uint8_t& flow_sources) const {
  auto edge_speed = tile->GetSpeed(edge, flow_mask_, seconds, false, &flow_sources);
  auto final_speed = std::min(edge_speed, top_speed_);

  float sec = (edge->length() * speedfactor_[final_speed]);

  if (shortest_) {
    return Cost(edge->length(), sec);
  }

  // Special case for travel on a ferry
  if (edge->use() == Use::kFerry) {
    // Use the edge speed (should be the speed of the ferry)
    return {sec * ferry_factor_, sec};
  }

  float factor = density_factor_[edge->density()] +
                 highway_factor_ * kHighwayFactor[static_cast<uint32_t>(edge->classification())] +
                 surface_factor_ * kSurfaceFactor[static_cast<uint32_t>(edge->surface())];
  // TODO: factor hasn't been extensively tested, might alter this in future
  float speed_penalty = (edge_speed > top_speed_) ? (edge_speed - top_speed_) * 0.05f : 0.0f;
  factor += speed_penalty;
  if (edge->toll()) {
    factor += toll_factor_;
  }

  if (edge->use() == Use::kTrack) {
    factor *= track_factor_;
  } else if (edge->use() == Use::kLivingStreet) {
    factor *= living_street_factor_;
  } else if (edge->use() == Use::kServiceRoad) {
    factor *= service_factor_;
  }

  return {sec * factor, sec};
}

// Returns the time (in seconds) to make the transition from the predecessor
Cost MotorcycleCost::TransitionCost(const baldr::DirectedEdge* edge,
                                    const baldr::NodeInfo* node,
                                    const EdgeLabel& pred) const {
  // Get the transition cost for country crossing, ferry, gate, toll booth,
  // destination only, alley, maneuver penalty
  uint32_t idx = pred.opp_local_idx();
  Cost c = base_transition_cost(node, edge, &pred, idx);
  c.secs = OSRMCarTurnDuration(edge, node, idx);

  // Transition time = turncost * stopimpact * densityfactor
  if (edge->stopimpact(idx) > 0 && !shortest_) {
    float turn_cost;
    if (edge->edge_to_right(idx) && edge->edge_to_left(idx)) {
      turn_cost = kTCCrossing;
    } else {
      turn_cost = (node->drive_on_right())
                      ? kRightSideTurnCosts[static_cast<uint32_t>(edge->turntype(idx))]
                      : kLeftSideTurnCosts[static_cast<uint32_t>(edge->turntype(idx))];
    }

    if ((edge->use() != Use::kRamp && pred.use() == Use::kRamp) ||
        (edge->use() == Use::kRamp && pred.use() != Use::kRamp)) {
      turn_cost += 1.5f;
      if (edge->roundabout())
        turn_cost += 0.5f;
    }

    float seconds = turn_cost;
    bool is_turn = false;
    bool has_left = (edge->turntype(idx) == baldr::Turn::Type::kLeft ||
                     edge->turntype(idx) == baldr::Turn::Type::kSharpLeft);
    bool has_right = (edge->turntype(idx) == baldr::Turn::Type::kRight ||
                      edge->turntype(idx) == baldr::Turn::Type::kSharpRight);
    bool has_reverse = edge->turntype(idx) == baldr::Turn::Type::kReverse;

    // Separate time and penalty when traffic is present. With traffic, edge speeds account for
    // much of the intersection transition time (TODO - evaluate different elapsed time settings).
    // Still want to add a penalty so routes avoid high cost intersections.
    if (has_left || has_right || has_reverse) {
      seconds *= edge->stopimpact(idx);
      is_turn = true;
    }

    AddUturnPenalty(idx, node, edge, has_reverse, has_left, has_right, false, InternalTurn::kNoTurn,
                    seconds);

    // Apply density factor and stop impact penalty if there isn't traffic on this edge or you're not
    // using traffic
    if (!pred.has_measured_speed()) {
      if (!is_turn)
        seconds *= edge->stopimpact(idx);
      seconds *= trans_density_factor_[node->density()];
    }
    c.cost += seconds;
  }
  return c;
}

// Returns the cost to make the transition from the predecessor edge
// when using a reverse search (from destination towards the origin).
// pred is the opposing current edge in the reverse tree
// edge is the opposing predecessor in the reverse tree
Cost MotorcycleCost::TransitionCostReverse(const uint32_t idx,
                                           const baldr::NodeInfo* node,
                                           const baldr::DirectedEdge* pred,
                                           const baldr::DirectedEdge* edge,
                                           const bool has_measured_speed,
                                           const InternalTurn /*internal_turn*/) const {

  // Motorcycles should be able to make uturns on short internal edges; therefore, InternalTurn
  // is ignored for now.
  // TODO: do we want to update the cost if we have flow or speed from traffic.

  // Get the transition cost for country crossing, ferry, gate, toll booth,
  // destination only, alley, maneuver penalty
  Cost c = base_transition_cost(node, edge, pred, idx);
  c.secs = OSRMCarTurnDuration(edge, node, pred->opp_local_idx());

  // Transition time = turncost * stopimpact * densityfactor
  if (edge->stopimpact(idx) > 0 && !shortest_) {
    float turn_cost;
    if (edge->edge_to_right(idx) && edge->edge_to_left(idx)) {
      turn_cost = kTCCrossing;
    } else {
      turn_cost = (node->drive_on_right())
                      ? kRightSideTurnCosts[static_cast<uint32_t>(edge->turntype(idx))]
                      : kLeftSideTurnCosts[static_cast<uint32_t>(edge->turntype(idx))];
    }

    if ((edge->use() != Use::kRamp && pred->use() == Use::kRamp) ||
        (edge->use() == Use::kRamp && pred->use() != Use::kRamp)) {
      turn_cost += 1.5f;
      if (edge->roundabout())
        turn_cost += 0.5f;
    }

    float seconds = turn_cost;
    bool is_turn = false;
    bool has_left = (edge->turntype(idx) == baldr::Turn::Type::kLeft ||
                     edge->turntype(idx) == baldr::Turn::Type::kSharpLeft);
    bool has_right = (edge->turntype(idx) == baldr::Turn::Type::kRight ||
                      edge->turntype(idx) == baldr::Turn::Type::kSharpRight);
    bool has_reverse = edge->turntype(idx) == baldr::Turn::Type::kReverse;

    // Separate time and penalty when traffic is present. With traffic, edge speeds account for
    // much of the intersection transition time (TODO - evaluate different elapsed time settings).
    // Still want to add a penalty so routes avoid high cost intersections.
    if (has_left || has_right || has_reverse) {
      seconds *= edge->stopimpact(idx);
      is_turn = true;
    }

    AddUturnPenalty(idx, node, edge, has_reverse, has_left, has_right, false, InternalTurn::kNoTurn,
                    seconds);

    // Apply density factor and stop impact penalty if there isn't traffic on this edge or you're not
    // using traffic
    if (!has_measured_speed) {
      if (!is_turn)
        seconds *= edge->stopimpact(idx);
      seconds *= trans_density_factor_[node->density()];
    }
    c.cost += seconds;
  }
  return c;
}

void ParseMotorcycleCostOptions(const rapidjson::Document& doc,
                                const std::string& costing_options_key,
                                CostingOptions* pbf_costing_options) {
  pbf_costing_options->set_costing(Costing::motorcycle);
  pbf_costing_options->set_name(Costing_Enum_Name(pbf_costing_options->costing()));
  auto json_costing_options = rapidjson::get_child_optional(doc, costing_options_key.c_str());

  if (json_costing_options) {
    // TODO: farm more common stuff out to parent class
    ParseSharedCostOptions(*json_costing_options, pbf_costing_options);

    // If specified, parse json and set pbf values

    // maneuver_penalty
    pbf_costing_options->set_maneuver_penalty(kManeuverPenaltyRange(
        rapidjson::get_optional<float>(*json_costing_options, "/maneuver_penalty")
            .get_value_or(kDefaultManeuverPenalty)));

    // destination_only_penalty
    pbf_costing_options->set_destination_only_penalty(kDestinationOnlyPenaltyRange(
        rapidjson::get_optional<float>(*json_costing_options, "/destination_only_penalty")
            .get_value_or(kDefaultDestinationOnlyPenalty)));

    // gate_cost
    pbf_costing_options->set_gate_cost(
        kGateCostRange(rapidjson::get_optional<float>(*json_costing_options, "/gate_cost")
                           .get_value_or(kDefaultGateCost)));

    // gate_penalty
    pbf_costing_options->set_gate_penalty(
        kGatePenaltyRange(rapidjson::get_optional<float>(*json_costing_options, "/gate_penalty")
                              .get_value_or(kDefaultGatePenalty)));

    // toll_booth_cost
    pbf_costing_options->set_toll_booth_cost(
        kTollBoothCostRange(rapidjson::get_optional<float>(*json_costing_options, "/toll_booth_cost")
                                .get_value_or(kDefaultTollBoothCost)));

    // toll_booth_penalty
    pbf_costing_options->set_toll_booth_penalty(kTollBoothPenaltyRange(
        rapidjson::get_optional<float>(*json_costing_options, "/toll_booth_penalty")
            .get_value_or(kDefaultTollBoothPenalty)));

    // alley_penalty
    pbf_costing_options->set_alley_penalty(
        kAlleyPenaltyRange(rapidjson::get_optional<float>(*json_costing_options, "/alley_penalty")
                               .get_value_or(kDefaultAlleyPenalty)));

    // country_crossing_cost
    pbf_costing_options->set_country_crossing_cost(kCountryCrossingCostRange(
        rapidjson::get_optional<float>(*json_costing_options, "/country_crossing_cost")
            .get_value_or(kDefaultCountryCrossingCost)));

    // country_crossing_penalty
    pbf_costing_options->set_country_crossing_penalty(kCountryCrossingPenaltyRange(
        rapidjson::get_optional<float>(*json_costing_options, "/country_crossing_penalty")
            .get_value_or(kDefaultCountryCrossingPenalty)));

    // ferry_cost
    pbf_costing_options->set_ferry_cost(
        kFerryCostRange(rapidjson::get_optional<float>(*json_costing_options, "/ferry_cost")
                            .get_value_or(kDefaultFerryCost)));

    // use_ferry
    pbf_costing_options->set_use_ferry(
        kUseFerryRange(rapidjson::get_optional<float>(*json_costing_options, "/use_ferry")
                           .get_value_or(kDefaultUseFerry)));

    // use_highways
    pbf_costing_options->set_use_highways(
        kUseHighwaysRange(rapidjson::get_optional<float>(*json_costing_options, "/use_highways")
                              .get_value_or(kDefaultUseHighways)));

    // use_tolls
    pbf_costing_options->set_use_tolls(
        kUseTollsRange(rapidjson::get_optional<float>(*json_costing_options, "/use_tolls")
                           .get_value_or(kDefaultUseTolls)));

    // use_trails
    pbf_costing_options->set_use_trails(
        kUseTrailsRange(rapidjson::get_optional<float>(*json_costing_options, "/use_trails")
                            .get_value_or(kDefaultUseTrails)));

    // use_tracks
    pbf_costing_options->set_use_tracks(
        kUseTracksRange(rapidjson::get_optional<float>(*json_costing_options, "/use_tracks")
                            .get_value_or(kDefaultUseTracks)));

    // use_living_street
    pbf_costing_options->set_use_living_streets(kUseLivingStreetsRange(
        rapidjson::get_optional<float>(*json_costing_options, "/use_living_streets")
            .get_value_or(kDefaultUseLivingStreets)));

    // service_penalty
    pbf_costing_options->set_service_penalty(
        kServicePenaltyRange(rapidjson::get_optional<float>(*json_costing_options, "/service_penalty")
                                 .get_value_or(kDefaultServicePenalty)));

    // service_factor
    pbf_costing_options->set_service_factor(
        kServiceFactorRange(rapidjson::get_optional<float>(*json_costing_options, "/service_factor")
                                .get_value_or(kDefaultServiceFactor)));
  } else {
    // Set pbf values to defaults
    pbf_costing_options->set_maneuver_penalty(kDefaultManeuverPenalty);
    pbf_costing_options->set_destination_only_penalty(kDefaultDestinationOnlyPenalty);
    pbf_costing_options->set_gate_cost(kDefaultGateCost);
    pbf_costing_options->set_gate_penalty(kDefaultGatePenalty);
    pbf_costing_options->set_toll_booth_cost(kDefaultTollBoothCost);
    pbf_costing_options->set_toll_booth_penalty(kDefaultTollBoothPenalty);
    pbf_costing_options->set_alley_penalty(kDefaultAlleyPenalty);
    pbf_costing_options->set_country_crossing_cost(kDefaultCountryCrossingCost);
    pbf_costing_options->set_country_crossing_penalty(kDefaultCountryCrossingPenalty);
    pbf_costing_options->set_ferry_cost(kDefaultFerryCost);
    pbf_costing_options->set_use_ferry(kDefaultUseFerry);
    pbf_costing_options->set_use_highways(kDefaultUseHighways);
    pbf_costing_options->set_use_tolls(kDefaultUseTolls);
    pbf_costing_options->set_use_trails(kDefaultUseTrails);
    pbf_costing_options->set_use_tracks(kDefaultUseTracks);
    pbf_costing_options->set_use_living_streets(kDefaultUseLivingStreets);
    pbf_costing_options->set_flow_mask(kDefaultFlowMask);
    pbf_costing_options->set_top_speed(kMaxAssumedSpeed);
    pbf_costing_options->set_service_penalty(kDefaultServicePenalty);
    pbf_costing_options->set_service_factor(kDefaultServiceFactor);
  }
}

cost_ptr_t CreateMotorcycleCost(const CostingOptions& costing_options) {
  return std::make_shared<MotorcycleCost>(costing_options);
}

} // namespace sif
} // namespace valhalla

/**********************************************************************************************/

#ifdef INLINE_TEST

using namespace valhalla;
using namespace sif;

namespace {

class TestMotorcycleCost : public MotorcycleCost {
public:
  TestMotorcycleCost(const CostingOptions& costing_options) : MotorcycleCost(costing_options){};

  using MotorcycleCost::alley_penalty_;
  using MotorcycleCost::country_crossing_cost_;
  using MotorcycleCost::destination_only_penalty_;
  using MotorcycleCost::ferry_transition_cost_;
  using MotorcycleCost::gate_cost_;
  using MotorcycleCost::maneuver_penalty_;
  using MotorcycleCost::service_factor_;
  using MotorcycleCost::service_penalty_;
  using MotorcycleCost::toll_booth_cost_;
};

TestMotorcycleCost* make_motorcyclecost_from_json(const std::string& property, float testVal) {
  std::stringstream ss;
  ss << R"({"costing_options":{"motorcycle":{")" << property << R"(":)" << testVal << "}}}";
  Api request;
  ParseApi(ss.str(), valhalla::Options::route, request);
  return new TestMotorcycleCost(
      request.options().costing_options(static_cast<int>(Costing::motorcycle)));
}

template <typename T>
std::uniform_real_distribution<T>* make_distributor_from_range(const ranged_default_t<T>& range) {
  T rangeLength = range.max - range.min;
  return new std::uniform_real_distribution<T>(range.min - rangeLength, range.max + rangeLength);
}

TEST(MotorcycleCost, testMotorcycleCostParams) {
  constexpr unsigned testIterations = 250;
  constexpr unsigned seed = 0;
  std::mt19937 generator(seed);
  std::shared_ptr<std::uniform_real_distribution<float>> distributor;
  std::shared_ptr<TestMotorcycleCost> ctorTester;

  // maneuver_penalty_
  distributor.reset(make_distributor_from_range(kManeuverPenaltyRange));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("maneuver_penalty", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->maneuver_penalty_,
                test::IsBetween(kManeuverPenaltyRange.min, kManeuverPenaltyRange.max));
  }

  // alley_penalty_
  distributor.reset(make_distributor_from_range(kAlleyPenaltyRange));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("alley_penalty", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->alley_penalty_,
                test::IsBetween(kAlleyPenaltyRange.min, kAlleyPenaltyRange.max));
  }

  // destination_only_penalty_
  distributor.reset(make_distributor_from_range(kDestinationOnlyPenaltyRange));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(
        make_motorcyclecost_from_json("destination_only_penalty", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->destination_only_penalty_,
                test::IsBetween(kDestinationOnlyPenaltyRange.min, kDestinationOnlyPenaltyRange.max));
  }

  // gate_cost_ (Cost.secs)
  distributor.reset(make_distributor_from_range(kGateCostRange));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("gate_cost", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->gate_cost_.secs, test::IsBetween(kGateCostRange.min, kGateCostRange.max));
  }

  // gate_penalty_ (Cost.cost)
  distributor.reset(make_distributor_from_range(kGatePenaltyRange));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("gate_penalty", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->gate_cost_.cost,
                test::IsBetween(kGatePenaltyRange.min, kGatePenaltyRange.max + kDefaultGateCost));
  }

  // toll_booth_cost_ (Cost.secs)
  distributor.reset(make_distributor_from_range(kTollBoothCostRange));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("toll_booth_cost", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->toll_booth_cost_.secs,
                test::IsBetween(kTollBoothCostRange.min, kTollBoothCostRange.max));
  }

  // tollbooth_penalty_ (Cost.cost)
  distributor.reset(make_distributor_from_range(kTollBoothPenaltyRange));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("toll_booth_penalty", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->toll_booth_cost_.cost,
                test::IsBetween(kTollBoothPenaltyRange.min,
                                kTollBoothPenaltyRange.max + kDefaultTollBoothCost));
  }

  // country_crossing_cost_ (Cost.secs)
  distributor.reset(make_distributor_from_range(kCountryCrossingCostRange));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(
        make_motorcyclecost_from_json("country_crossing_cost", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->country_crossing_cost_.secs,
                test::IsBetween(kCountryCrossingCostRange.min, kCountryCrossingCostRange.max));
  }

  // country_crossing_penalty_ (Cost.cost)
  distributor.reset(make_distributor_from_range(kCountryCrossingPenaltyRange));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(
        make_motorcyclecost_from_json("country_crossing_penalty", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->country_crossing_cost_.cost,
                test::IsBetween(kCountryCrossingPenaltyRange.min,
                                kCountryCrossingPenaltyRange.max + kDefaultCountryCrossingCost));
  }

  // ferry_cost_ (Cost.secs)
  distributor.reset(make_distributor_from_range(kFerryCostRange));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("ferry_cost", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->ferry_transition_cost_.secs,
                test::IsBetween(kFerryCostRange.min, kFerryCostRange.max));
  }

  // service_penalty_
  distributor.reset(make_distributor_from_range(kServicePenaltyRange));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("service_penalty", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->service_penalty_,
                test::IsBetween(kServicePenaltyRange.min, kServicePenaltyRange.max));
  }

  // service_factor_
  distributor.reset(make_distributor_from_range(kServiceFactorRange));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("service_factor", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->service_factor_,
                test::IsBetween(kServiceFactorRange.min, kServiceFactorRange.max));
  }

  /*
   // use_ferry
   distributor.reset(make_distributor_from_range(kUseFerryRange));
   for (unsigned i = 0; i < testIterations; ++i) {
     ctorTester.reset(make_motorcyclecost_from_json("use_ferry", (*distributor)(generator)));
EXPECT_THAT(ctorTester->use_ferry , test::IsBetween( kUseFerryRange.min ,kUseFerryRange.max));
   }

    // use_highways
    distributor.reset(make_distributor_from_range(kUseHighwaysRange));
    for (unsigned i = 0; i < testIterations; ++i) {
      ctorTester.reset(make_motorcyclecost_from_json("use_highways", (*distributor)(generator)));
EXPECT_THAT(ctorTester->use_highways , test::IsBetween( kUseHighwaysRange.min
,kUseHighwaysRange.max));
    }

     // use_trails
     distributor.reset(make_distributor_from_range(kUseTrailsRange));
     for (unsigned i = 0; i < testIterations; ++i) {
       ctorTester.reset(make_motorcyclecost_from_json("use_trails", (*distributor)(generator)));
EXPECT_THAT(ctorTester->use_trails , test::IsBetween( kUseTrailsRange.min ,kUseTrailsRange.max));
     }

   // use_tolls
   distributor.reset(make_distributor_from_range(kUseTollsRange));
   for (unsigned i = 0; i < testIterations; ++i) {
     ctorTester.reset(make_motorcyclecost_from_json("use_tolls", (*distributor)(generator)));
EXPECT_THAT(ctorTester->use_tolls , test::IsBetween( kUseTollsRange.min ,kUseTollsRange.max));
   }
   **/
}
} // namespace

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#endif
