//////////////////////////////////////////////////////////////////////////////
// BSD 3-Clause License
//
// Copyright (c) 2022, The Regents of the University of California
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "power_cells.h"
#include "domain.h"
#include "grid.h"
#include "straps.h"

#include "odb/db.h"
#include "utl/Logger.h"

namespace pdn {

PowerCell::PowerCell(utl::Logger* logger,
                     odb::dbMaster* master,
                     odb::dbMTerm* control,
                     odb::dbMTerm* acknowledge,
                     odb::dbMTerm* switched_power,
                     odb::dbMTerm* alwayson_power,
                     odb::dbMTerm* ground)
    : logger_(logger),
      master_(master),
      control_(control),
      acknowledge_(acknowledge),
      switched_power_(switched_power),
      alwayson_power_(alwayson_power),
      ground_(ground),
      alwayson_power_positions_()
{
}

const std::string PowerCell::getName() const
{
  return master_->getName();
}

void PowerCell::report() const
{
  logger_->info(utl::PDN, 200, "Switched power cell: {}", master_->getName());
  logger_->info(utl::PDN, 201, "  Control pin: {}", control_->getName());
  if (acknowledge_ != nullptr) {
    logger_->info(utl::PDN, 202, "  Acknowledge pin: {}", acknowledge_->getName());
  }
  logger_->info(utl::PDN, 203, "  Switched power pin: {}", switched_power_->getName());
  logger_->info(utl::PDN, 204, "  Always on power pin: {}", alwayson_power_->getName());
  logger_->info(utl::PDN, 205, "  Ground pin: {}", ground_->getName());
}

void PowerCell::populateAlwaysOnPinPositions(int site_width)
{
  alwayson_power_positions_.clear();

  for (auto* pin : alwayson_power_->getMPins()) {
    for (auto* box : pin->getGeometry()) {
      odb::Rect bbox;
      box->getBox(bbox);

      const auto pin_pos = getRectAsSiteWidths(bbox, site_width, 0);
      alwayson_power_positions_.insert(pin_pos.begin(), pin_pos.end());
    }
  }
}

std::set<int> PowerCell::getRectAsSiteWidths(const odb::Rect& rect, int site_width, int offset)
{
  std::set<int> pos;
  const int x_start = std::ceil(static_cast<double>(rect.xMin() - offset) / site_width) * site_width;
  const int x_end = std::floor(static_cast<double>(rect.xMax() - offset) / site_width) * site_width;
  for (int x = x_start; x <= x_end; x += site_width) {
    pos.insert(x + offset);
  }
  return pos;
}

//////////

GridSwitchedPower::GridSwitchedPower(Grid* grid,
                                     PowerCell* cell,
                                     odb::dbNet* control,
                                     NetworkType network)
  : grid_(grid),
    cell_(cell),
    control_(control),
    network_(network)
{
  if (network_ == DAISY && !cell->hasAcknowledge()) {
    grid->getLogger()->error(utl::PDN, 198, "{} requires the power cell to have an acknowledge pin.", toString(DAISY));
  }
}

const std::string GridSwitchedPower::toString(NetworkType type)
{
  switch (type) {
  case STAR:
    return "STAR";
  case DAISY:
    return "DAISY";
  }
  return "unknown";
}

GridSwitchedPower::NetworkType GridSwitchedPower::fromString(const std::string& type, utl::Logger* logger)
{
  if (type == "STAR") {
    return STAR;
  }
  if (type == "DAISY") {
    return DAISY;
  }

  logger->error(utl::PDN, 197, "Unrecognized network type: {}", type);
  return STAR;
}

void GridSwitchedPower::report() const
{
  auto* logger = grid_->getLogger();
  logger->info(utl::PDN, 210, "Switched power cell: {}", cell_->getName());
  logger->info(utl::PDN, 211, "  Control net: {}", control_->getName());
  logger->info(utl::PDN, 212, "  Network type: {}", toString(network_));
}

void GridSwitchedPower::build()
{
  if (!insts_.empty()) {
    // power switches already built and need to be ripped up to try again
    return;
  }

  odb::Rect core_area;
  grid_->getBlock()->getCoreArea(core_area);

  InstTree exisiting_insts;
  for (auto* inst : grid_->getBlock()->getInsts()) {
    if (!inst->getPlacementStatus().isFixed()) {
      continue;
    }

    odb::Rect bbox;
    inst->getBBox()->getBox(bbox);

    exisiting_insts.insert({Shape::rectToBox(bbox), inst});
  }

  auto* target = getLowestStrap();
  if (target == nullptr) {
    grid_->getLogger()->error(utl::PDN, 220, "Unable to find a strap to connect power switched to.");
  }

  odb::dbNet* switched = grid_->getDomain()->getSwitchedPower();
  odb::dbNet* alwayson = grid_->getDomain()->getAlwaysOnPower();
  odb::dbNet* ground = grid_->getDomain()->getGround();

  const auto& target_shapes = target->getShapes();
  ShapeTree targets;
  for (const auto& [box, shape] : target_shapes.at(target->getLayer())) {
    if (shape->getNet() != alwayson) {
      continue;
    }
    targets.insert({box, shape});
  }

  const auto rows = grid_->getDomain()->getRows();
  bgi::rtree<std::pair<Box, odb::dbRow*>, bgi::quadratic<16>> row_search;
  for (auto* row : rows) {
    odb::Rect bbox;
    row->getBBox(bbox);

    row_search.insert({Shape::rectToBox(bbox), row});
  }
  auto get_instance_rows = [&row_search] (odb::dbInst* inst) -> std::set<odb::dbRow*> {
    std::set<odb::dbRow*> rows;

    odb::Rect box;
    inst->getBBox()->getBox(box);

    for (auto itr = row_search.qbegin(bgi::intersects(Shape::rectToBox(box)));
         itr != row_search.qend();
         itr++) {
      auto* row = itr->second;
      odb::Rect row_box;
      row->getBBox(row_box);

      const auto overlap = row_box.intersect(box);
      if (overlap.minDXDY() != 0) {
        rows.insert(row);
      }
    }

    return rows;
  };

  for (auto* row : rows) {
    const int site_width = row->getSite()->getWidth();
    cell_->populateAlwaysOnPinPositions(site_width);
    if (row->getOrient() == odb::dbOrientType::R0) {
      continue;
    }
    const std::string inst_prefix = inst_prefix_ + row->getName() + "_";
    int idx = 0;

    debugPrint(grid_->getLogger(), utl::PDN, "PowerSwitch", 2, "Adding power switches in row: {}", row->getName());

    odb::Rect bbox;
    row->getBBox(bbox);
    std::vector<odb::Rect> straps;
    for (auto itr = targets.qbegin(bgi::intersects(Shape::rectToBox(bbox)));
         itr != targets.qend();
         itr++) {
      const auto& shape = itr->second;
      straps.push_back(shape->getRect());
    }

    std::sort(straps.begin(), straps.end(), [](const odb::Rect& lhs, const odb::Rect& rhs) {
      return lhs.xMin() < rhs.xMin();
    });

    for (const auto& strap : straps) {
      const std::string new_name = inst_prefix + std::to_string(idx);
      auto* inst = odb::dbInst::create(grid_->getBlock(),
                                       cell_->getMaster(),
                                       new_name.c_str(),
                                       true);
      if (inst == nullptr) {
        inst = grid_->getBlock()->findInst(new_name.c_str());
        if (inst->getMaster() != cell_->getMaster()) {
          grid_->getLogger()->error(utl::PDN,
                                    221,
                                    "Instance {} should be {}, but is {}.",
                                    new_name,
                                    cell_->getMaster()->getName(),
                                    inst->getMaster()->getName());
        }
      }

      debugPrint(grid_->getLogger(), utl::PDN, "PowerSwitch", 3, "Adding switch {}", new_name);

      const auto locations = computeLocations(strap, site_width, core_area);
      inst->setLocation(*locations.begin(), bbox.yMin());
      inst->setPlacementStatus(odb::dbPlacementStatus::FIRM);

      const auto inst_rows = get_instance_rows(inst);
      if (inst_rows.size() < 2) {
        // inst is not in multiple rows, so remove
        odb::dbInst::destroy(inst);
        debugPrint(grid_->getLogger(), utl::PDN, "PowerSwitch", 3, "Removing switch {} since it is not inside two twos.", new_name);
        continue;
      }

      inst->getITerm(cell_->getGroundPin())->connect(ground);
      inst->getITerm(cell_->getAlwaysOnPowerPin())->connect(alwayson);
      inst->getITerm(cell_->getSwitchedPowerPin())->connect(switched);

      insts_[inst] = InstanceInfo{locations, inst_rows};

      idx++;
    }
  }

  switch(network_) {
  case STAR:
    updateControlNetworkSTAR();
    break;
  case DAISY:
    updateControlNetworkDAISY(true);
    break;
  }

  checkAndFixOverlappingInsts(exisiting_insts);
}

void GridSwitchedPower::updateControlNetworkSTAR()
{
  for (const auto& [inst, inst_info] : insts_) {
    inst->getITerm(cell_->getControlPin())->connect(control_);
  }
}

void GridSwitchedPower::updateControlNetworkDAISY(const bool order_by_x)
{
  std::map<int, std::vector<odb::dbInst*>> inst_order;

  for (const auto& [inst, inst_info] : insts_) {
    int loc;
    int x, y;
    inst->getLocation(x, y);
    if (order_by_x) {
      loc = x;
    } else {
      loc = y;
    }

    inst_order[loc].push_back(inst);
  }

  for (auto& [pos, insts] : inst_order) {
    std::sort(insts.begin(), insts.end(), [order_by_x](odb::dbInst* lhs, odb::dbInst* rhs) {
      int lhs_x, lhs_y;
      lhs->getLocation(lhs_x, lhs_y);
      int rhs_x, rhs_y;
      rhs->getLocation(rhs_x, rhs_y);

      if (order_by_x) {
        return lhs_y < rhs_y;
      } else {
        return lhs_x < rhs_x;
      }
    });
  }

  auto get_next_ack = [this](const std::string& inst_name) {
    std::string net_name = inst_name + "_" + cell_->getAcknowledgePin()->getName();
    auto* ack = odb::dbNet::create(grid_->getBlock(), net_name.c_str());
    if (ack == nullptr) {
      return grid_->getBlock()->findNet(net_name.c_str());
    } else {
      return ack;
    }
  };

  odb::dbNet* control = control_;
  for (const auto& [pos, insts] : inst_order) {
    odb::dbNet* next_control = nullptr;
    for (auto* inst : insts) {
      odb::dbNet* ack = get_next_ack(inst->getName());

      inst->getITerm(cell_->getControlPin())->connect(control);
      inst->getITerm(cell_->getAcknowledgePin())->connect(ack);

      control = ack;
      if (next_control == nullptr) {
        next_control = ack;
      }
    }
    control = next_control;
  }

  // remove dangling signals
  for (const auto& [inst, inst_info] : insts_) {
    odb::dbNet* net = inst->getITerm(cell_->getAcknowledgePin())->getNet();
    if (net == nullptr) {
      continue;
    }
    if (net->getITermCount() < 2) {
      odb::dbNet::destroy(net);
    }
  }
}

void GridSwitchedPower::checkAndFixOverlappingInsts(const InstTree& insts)
{
  // needs to check for bounds of the rows
  for (const auto& [inst, inst_info] : insts_) {
    auto* overlapping = checkOverlappingInst(inst, insts);
    if (overlapping == nullptr) {
      continue;
    }
    debugPrint(grid_->getLogger(),
               utl::PDN,
               "PowerSwitch",
               2,
               "Power switch {} overlaps with {}",
               inst->getName(),
               overlapping->getName());

    int x, y;
    inst->getLocation(x, y);
    bool fixed = false;
    // start by checking if this can be resolved by moving the power switch
    for (int new_pos : inst_info.sites) {
      if (new_pos == x) {
        continue;
      }

      inst->setLocation(new_pos, y);
      if (checkInstanceOverlap(inst, overlapping)) {
        debugPrint(grid_->getLogger(),
                   utl::PDN,
                   "PowerSwitch",
                   3,
                   "Fixed by moving {} to ({}, {})",
                   inst->getName(),
                   new_pos / static_cast<double>(grid_->getBlock()->getDbUnitsPerMicron()),
                   y / static_cast<double>(grid_->getBlock()->getDbUnitsPerMicron()));
        fixed = true;
        break;
      }
    }
    if (fixed) {
      continue;
    }
    // restore original position
    inst->setLocation(x, y);
    // next find minimum shift of other cell
    const int pws_min = *inst_info.sites.begin();
    const int pws_max = *inst_info.sites.rbegin();
    const int pws_width = cell_->getMaster()->getWidth();

    int overlap_y;
    overlapping->getLocation(x, overlap_y);
    const int other_width = overlapping->getMaster()->getWidth();

    const int other_avg = x + other_width / 2;
    const int pws_min_avg = pws_min + pws_width / 2;
    const int pws_max_avg = pws_max + pws_width / 2;

    const int pws_min_displacement = std::abs(pws_min_avg - other_avg);
    const int pws_max_displacement = std::abs(pws_max_avg - other_avg);

    int pws_new_loc, other_new_loc;
    if (pws_min_displacement < pws_max_displacement) {
      pws_new_loc = pws_min;
      other_new_loc = pws_new_loc + pws_width;
    } else {
      pws_new_loc = pws_max;
      other_new_loc = pws_new_loc - other_width;
    }

    inst->setLocation(pws_new_loc, y);
    overlapping->setLocation(other_new_loc, overlap_y);
    debugPrint(grid_->getLogger(),
               utl::PDN,
               "PowerSwitch",
               3,
               "Fixed by moving {} to ({}, {}) and {} to ({}, {})",
               inst->getName(),
               pws_new_loc, y,
               overlapping->getName(),
               other_new_loc, overlap_y);
    fixed = true;
  }
}

bool GridSwitchedPower::checkInstanceOverlap(odb::dbInst* inst0, odb::dbInst* inst1) const
{
  odb::Rect inst0_bbox;
  inst0->getBBox()->getBox(inst0_bbox);

  odb::Rect inst1_bbox;
  inst1->getBBox()->getBox(inst1_bbox);

  const odb::Rect overlap = inst0_bbox.intersect(inst1_bbox);
  if (overlap.isInverted()) {
    return true;
  }
  return overlap.area() == 0;
}

odb::dbInst* GridSwitchedPower::checkOverlappingInst(odb::dbInst* cell, const InstTree& insts) const
{
  odb::Rect bbox;
  cell->getBBox()->getBox(bbox);

  for (auto itr = insts.qbegin(bgi::intersects(Shape::rectToBox(bbox)));
       itr != insts.qend();
       itr++) {
    auto* other_inst = itr->second;
    if (!checkInstanceOverlap(cell, other_inst)) {
      return other_inst;
    }
  }

  return nullptr;
}

void GridSwitchedPower::ripup()
{
  for (const auto& [inst, inst_info] : insts_) {
    if (cell_->hasAcknowledge()) {
      auto* net = inst->getITerm(cell_->getAcknowledgePin())->getNet();
      if (net != nullptr) {
        odb::dbNet::destroy(net);
      }
    }

    odb::dbInst::destroy(inst);
  }
  insts_.clear();
}

Straps* GridSwitchedPower::getLowestStrap() const
{
  Straps* target = nullptr;

  for (const auto& strap : grid_->getStraps()) {
    if (strap->type() != GridComponent::Strap) {
      continue;
    }

    if (target == nullptr) {
      target = strap.get();
    } else {
      const int target_level = target->getLayer()->getRoutingLevel();
      const int strap_level = strap->getLayer()->getRoutingLevel();

      if (target_level == strap_level) {
        if (target->getShapeCount() < strap->getShapeCount()) {
          // use the one with more shapes
          target = strap.get();
        }
      } else if (target_level > strap_level) {
        target = strap.get();
      }
    }
  }

  return target;
}

const ShapeTreeMap GridSwitchedPower::getShapes() const
{
  ShapeTreeMap shapes;

  for (const auto& [inst, inst_info] : insts_) {
    for (const auto& [layer, inst_shapes] : InstanceGrid::getInstancePins(inst)) {
      auto& layer_shapes = shapes[layer];
      layer_shapes.insert(inst_shapes.begin(), inst_shapes.end());
    }
  }

  return shapes;
}

std::set<int> GridSwitchedPower::computeLocations(const odb::Rect& strap,
                                                  int site_width,
                                                  const odb::Rect& corearea) const
{
  const auto& pin_pos = cell_->getAlwaysOnPowerPinPositions();
  const int min_pin = *pin_pos.begin();
  const int max_pin = *pin_pos.rbegin();

  std::set<int> pos;
  for (auto strap_pos : PowerCell::getRectAsSiteWidths(strap, site_width, corearea.xMin())) {
    for (auto pin : cell_->getAlwaysOnPowerPinPositions()) {
      const int new_pos = strap_pos - pin;

      const int new_min_pin = new_pos + min_pin;
      const int new_max_pin = new_pos + max_pin;

      if (new_min_pin >= strap.xMin() && new_max_pin <= strap.xMax()) {
        // pin is completely inside strap
        pos.insert(new_pos);
      } else if (new_min_pin <= strap.xMin() && new_max_pin >= strap.xMax()) {
        // pin is completely overlapping strap
        pos.insert(new_pos);
      }
    }
  }

  return pos;
}

}  // namespace pdn
