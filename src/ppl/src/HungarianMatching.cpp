/////////////////////////////////////////////////////////////////////////////
//
// BSD 3-Clause License
//
// Copyright (c) 2019, The Regents of the University of California
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
//
///////////////////////////////////////////////////////////////////////////////

#include "HungarianMatching.h"

#include "utl/Logger.h"

namespace ppl {

HungarianMatching::HungarianMatching(Section& section,
                                     Netlist* netlist,
                                     Core* core,
                                     std::vector<Slot>& slots,
                                     Logger* logger,
                                     odb::dbDatabase* db)
    : netlist_(netlist),
      core_(core),
      pin_indices_(section.pin_indices),
      pin_groups_(section.pin_groups),
      slots_(slots),
      db_(db)
{
  num_io_pins_ = section.pin_indices.size();
  num_pin_groups_ = netlist_->numIOGroups();
  begin_slot_ = section.begin_slot;
  end_slot_ = section.end_slot;
  num_slots_ = end_slot_ - begin_slot_;
  non_blocked_slots_ = section.num_slots;
  group_slots_ = 0;
  group_size_ = -1;
  edge_ = section.edge;
  logger_ = logger;
}

void HungarianMatching::findAssignment()
{
  createMatrix();
  if (!hungarian_matrix_.empty())
    hungarian_solver_.solve(hungarian_matrix_, assignment_);
}

void HungarianMatching::createMatrix()
{
  hungarian_matrix_.resize(non_blocked_slots_);
  int slot_index = 0;
  for (int i = begin_slot_; i <= end_slot_; ++i) {
    int pinIndex = 0;
    Point newPos = slots_[i].pos;
    if (slots_[i].blocked) {
      continue;
    }
    hungarian_matrix_[slot_index].resize(num_io_pins_,
                                         std::numeric_limits<int>::max());
    for (int idx : pin_indices_) {
      const IOPin& io_pin = netlist_->getIoPin(idx);
      if (!io_pin.isInGroup()) {
        int hpwl = netlist_->computeIONetHPWL(idx, newPos);
        hungarian_matrix_[slot_index][pinIndex] = hpwl;
        pinIndex++;
      }
    }
    slot_index++;
  }
}

inline bool samePos(Point& a, Point& b)
{
  return (a.x() == b.x() && a.y() == b.y());
}

void HungarianMatching::getFinalAssignment(std::vector<IOPin>& assignment,
                                           MirroredPins& mirrored_pins,
                                           bool assign_mirrored) const
{
  size_t rows = non_blocked_slots_;
  size_t col = 0;
  int slot_index = 0;
  for (int idx : pin_indices_) {
    IOPin& io_pin = netlist_->getIoPin(idx);

    if (!io_pin.isInGroup()) {
      slot_index = begin_slot_;
      for (size_t row = 0; row < rows; row++) {
        while (slots_[slot_index].blocked && slot_index < slots_.size())
          slot_index++;
        if (assignment_[row] != col) {
          slot_index++;
          continue;
        }
        if (hungarian_matrix_[row][col] == hungarian_fail) {
          logger_->warn(utl::PPL,
                        33,
                        "I/O pin {} cannot be placed in the specified region. "
                        "Not enough space.",
                        io_pin.getName().c_str());
        }

        // Make this check here to avoid messing up the correlation between the
        // pin sorting and the hungarian matrix values
        if ((assign_mirrored
             && mirrored_pins.find(io_pin.getBTerm()) == mirrored_pins.end())
            || io_pin.isPlaced()) {
          continue;
        }
        io_pin.setPos(slots_[slot_index].pos);
        io_pin.setLayer(slots_[slot_index].layer);
        io_pin.setPlaced();
        assignment.push_back(io_pin);
        slots_[slot_index].used = true;

        if (assign_mirrored) {
          odb::dbBTerm* mirrored_term = mirrored_pins[io_pin.getBTerm()];
          int mirrored_pin_idx = netlist_->getIoPinIdx(mirrored_term);
          IOPin& mirrored_pin = netlist_->getIoPin(mirrored_pin_idx);

          odb::Point mirrored_pos = core_->getMirroredPosition(io_pin.getPos());
          mirrored_pin.setPos(mirrored_pos);
          mirrored_pin.setLayer(slots_[slot_index].layer);
          mirrored_pin.setPlaced();
          assignment.push_back(mirrored_pin);
          slot_index
              = getSlotIdxByPosition(mirrored_pos, mirrored_pin.getLayer());
          if (slot_index < 0) {
            odb::dbTechLayer* layer
                = db_->getTech()->findRoutingLayer(mirrored_pin.getLayer());
            logger_->error(utl::PPL,
                           82,
                           "Mirrored position ({}, {}) at layer {} is not a "
                           "valid position for pin placement.",
                           mirrored_pos.getX(),
                           mirrored_pos.getY(),
                           layer->getName());
          }
          slots_[slot_index].used = true;
        }
        break;
      }
      col++;
    }
  }
}

void HungarianMatching::findAssignmentForGroups()
{
  createMatrixForGroups();

  if (!hungarian_matrix_.empty())
    hungarian_solver_.solve(hungarian_matrix_, assignment_);
}

void HungarianMatching::createMatrixForGroups()
{
  for (const auto& [pins, order] : pin_groups_) {
    group_size_ = std::max(static_cast<int>(pins.size()), group_size_);
  }

  if (group_size_ > 0) {
    // end the loop when i > (end_slot_ - group_size_ + 1)
    // to avoid access invalid positions of slots_.
    for (int i = begin_slot_; i <= (end_slot_ - group_size_ + 1);
         i += group_size_) {
      bool blocked = false;
      for (int pin_cnt = 0; pin_cnt < group_size_; pin_cnt++) {
        if (slots_[i + pin_cnt].blocked) {
          blocked = true;
        }
      }
      if (!blocked) {
        group_slots_++;
      }
    }

    hungarian_matrix_.resize(group_slots_);
    int slot_index = 0;
    // end the loop when i > (end_slot_ - group_size_ + 1)
    // to avoid access invalid positions of slots_.
    for (int i = begin_slot_; i <= (end_slot_ - group_size_ + 1);
         i += group_size_) {
      int groupIndex = 0;
      Point newPos = slots_[i].pos;

      bool blocked = false;
      for (int pin_cnt = 0; pin_cnt < group_size_; pin_cnt++) {
        if (slots_[i + pin_cnt].blocked) {
          blocked = true;
        }
      }
      if (blocked) {
        continue;
      }

      hungarian_matrix_[slot_index].resize(num_pin_groups_,
                                           std::numeric_limits<int>::max());
      for (const auto& [pins, order] : pin_groups_) {
        int group_hpwl = 0;
        for (const int io_idx : pins) {
          int pin_hpwl = netlist_->computeIONetHPWL(io_idx, newPos);
          if (pin_hpwl == hungarian_fail) {
            group_hpwl = hungarian_fail;
            break;
          } else {
            group_hpwl += pin_hpwl;
          }
        }
        hungarian_matrix_[slot_index][groupIndex] = group_hpwl;
        groupIndex++;
      }
      slot_index++;
    }
  }
}

void HungarianMatching::getAssignmentForGroups(std::vector<IOPin>& assignment)
{
  if (hungarian_matrix_.size() <= 0)
    return;

  size_t rows = group_slots_;
  size_t col = 0;
  int slot_index = 0;
  for (const auto& [pins, order] : pin_groups_) {
    slot_index = begin_slot_;
    for (size_t row = 0; row < rows; row++) {
      while (slots_[slot_index].blocked && slot_index < slots_.size())
        slot_index += group_size_;
      if (assignment_[row] != col) {
        slot_index += group_size_;
        continue;
      }

      int pin_cnt = (edge_ == Edge::top || edge_ == Edge::left) && order
                        ? pins.size() - 1
                        : 0;

      for (int pin_idx : pins) {
        IOPin& io_pin = netlist_->getIoPin(pin_idx);
        io_pin.setPos(slots_[slot_index + pin_cnt].pos);
        io_pin.setLayer(slots_[slot_index + pin_cnt].layer);
        assignment.push_back(io_pin);
        slots_[slot_index + pin_cnt].used = true;
        slots_[slot_index + pin_cnt].blocked = true;
        if ((slot_index + pin_cnt) <= end_slot_)
          non_blocked_slots_--;
        pin_cnt = (edge_ == Edge::top || edge_ == Edge::left) && order
                      ? pin_cnt - 1
                      : pin_cnt + 1;
      }
      break;
    }
    col++;
  }

  hungarian_matrix_.clear();
  assignment_.clear();
}

int HungarianMatching::getSlotIdxByPosition(const odb::Point& position,
                                            int layer) const
{
  int slot_idx = -1;
  for (int i = 0; i < slots_.size(); i++) {
    if (slots_[i].pos == position && slots_[i].layer == layer) {
      slot_idx = i;
      break;
    }
  }

  return slot_idx;
}

}  // namespace ppl
