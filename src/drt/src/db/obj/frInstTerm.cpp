/* Authors: Matt Liberty */
/*
 * Copyright (c) 2021, The Regents of the University of California
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the University nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "db/obj/frInstTerm.h"

#include "db/obj/frInst.h"

namespace drt {

frString frInstTerm::getName() const
{
  return getInst()->getName() + '/' + getTerm()->getName();
}

frAccessPoint* frInstTerm::getAccessPoint(frCoord x, frCoord y, frLayerNum lNum)
{
  auto inst = getInst();
  dbTransform shiftXform = inst->getTransform();
  Point offset(shiftXform.getOffset());
  x = x - offset.getX();
  y = y - offset.getY();
  return term_->getAccessPoint(x, y, lNum, inst->getPinAccessIdx());
}

bool frInstTerm::hasAccessPoint(frCoord x, frCoord y, frLayerNum lNum)
{
  return getAccessPoint(x, y, lNum) != nullptr;
}

void frInstTerm::getShapes(std::vector<frRect>& outShapes,
                           bool updatedTransform)
{
  term_->getShapes(outShapes);
  for (auto& shape : outShapes) {
    dbTransform trans;
    if (updatedTransform) {
      trans = getInst()->getUpdatedXform();
    } else {
      trans = getInst()->getTransform();
    }
    shape.move(trans);
  }
}

Rect frInstTerm::getBBox(bool updatedTransform)
{
  Rect bbox(term_->getBBox());
  dbTransform trans;
  if (updatedTransform) {
    trans = getInst()->getUpdatedXform();
  } else {
    trans = getInst()->getTransform();
  }
  trans.apply(bbox);
  return bbox;
}

}  // namespace drt
