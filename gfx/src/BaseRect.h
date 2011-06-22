/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Corporation code.
 *
 * The Initial Developer of the Original Code is Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2011
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Robert O'Callahan <robert@ocallahan.org>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#ifndef MOZILLA_BASERECT_H_
#define MOZILLA_BASERECT_H_

#include "nsAlgorithm.h"

namespace mozilla {

/**
 * Rectangles have two interpretations: a set of (zero-size) points,
 * and a rectangular area of the plane. Most rectangle operations behave
 * the same no matter what interpretation is being used, but some operations
 * differ:
 * -- Equality tests behave differently. When a rectangle represents an area,
 * all zero-width and zero-height rectangles are equal to each other since they
 * represent the empty area. But when a rectangle represents a set of
 * mathematical points, zero-width and zero-height rectangles can be unequal.
 * -- The union operation can behave differently. When rectangles represent
 * areas, taking the union of a zero-width or zero-height rectangle with
 * another rectangle can just ignore the empty rectangle. But when rectangles
 * represent sets of mathematical points, we may need to extend the latter
 * rectangle to include the points of a zero-width or zero-height rectangle.
 *
 * To ensure that these interpretations are explicitly disambiguated, we
 * deny access to the == and != operators and require use of IsEqualEdges and
 * IsEqualInterior instead. Similarly we provide separate Union and UnionEdges
 * methods.
 *
 * Do not use this class directly. Subclass it, pass that subclass as the
 * Sub parameter, and only use that subclass.
 */
template <class T, class Sub, class Point, class SizeT, class Margin>
struct BaseRect {
  T x, y, width, height;

  // Constructors
  BaseRect() : x(0), y(0), width(0), height(0) {}
  BaseRect(const Point& aOrigin, const SizeT &aSize) :
      x(aOrigin.x), y(aOrigin.y), width(aSize.width), height(aSize.height)
  {
  }
  BaseRect(T aX, T aY, T aWidth, T aHeight) :
      x(aX), y(aY), width(aWidth), height(aHeight)
  {
  }

  // Emptiness. An empty rect is one that has no area, i.e. its height or width
  // is <= 0
  bool IsEmpty() const { return height <= 0 || width <= 0; }
  void SetEmpty() { width = height = 0; }

  // Returns true if this rectangle contains the interior of aRect. Always
  // returns true if aRect is empty, and always returns false is aRect is
  // nonempty but this rect is empty.
  bool Contains(const Sub& aRect) const
  {
    return aRect.IsEmpty() ||
           (x <= aRect.x && aRect.XMost() <= XMost() &&
            y <= aRect.y && aRect.YMost() <= YMost());
  }
  // Returns true if this rectangle contains the rectangle (aX,aY,1,1).
  bool Contains(T aX, T aY) const
  {
    return x <= aX && aX + 1 <= XMost() &&
           y <= aY && aY + 1 <= YMost();
  }
  // Returns true if this rectangle contains the rectangle (aPoint.x,aPoint.y,1,1).
  bool Contains(const Point& aPoint) const { return Contains(aPoint.x, aPoint.y); }

  // Intersection. Returns TRUE if the receiver's area has non-empty
  // intersection with aRect's area, and FALSE otherwise.
  // Always returns false if aRect is empty or 'this' is empty.
  bool Intersects(const Sub& aRect) const
  {
    return x < aRect.XMost() && aRect.x < XMost() &&
           y < aRect.YMost() && aRect.y < YMost();
  }
  // Returns the rectangle containing the intersection of the points
  // (including edges) of *this and aRect. If there are no points in that
  // intersection, returns an empty rectangle with x/y set to the max of the x/y
  // of *this and aRect.
  Sub Intersect(const Sub& aRect) const
  {
    Sub result;
    result.x = NS_MAX(x, aRect.x);
    result.y = NS_MAX(y, aRect.y);
    result.width = NS_MIN(XMost(), aRect.XMost()) - result.x;
    result.height = NS_MIN(YMost(), aRect.YMost()) - result.y;
    if (result.width < 0 || result.height < 0) {
      result.SizeTo(0, 0);
    }
    return result;
  }
  // Sets *this to be the rectangle containing the intersection of the points
  // (including edges) of *this and aRect. If there are no points in that
  // intersection, sets *this to be an empty rectangle with x/y set to the max
  // of the x/y of *this and aRect.
  //
  // 'this' can be the same object as either aRect1 or aRect2
  bool IntersectRect(const Sub& aRect1, const Sub& aRect2)
  {
    *static_cast<Sub*>(this) = aRect1.Intersect(aRect2);
    return !IsEmpty();
  }

  // Returns the smallest rectangle that contains both the area of both
  // this and aRect2.
  // Thus, empty input rectangles are ignored.
  // If both rectangles are empty, returns this.
  Sub Union(const Sub& aRect) const
  {
    if (IsEmpty()) {
      return aRect;
    } else if (aRect.IsEmpty()) {
      return *static_cast<const Sub*>(this);
    } else {
      return UnionEdges(aRect);
    }
  }
  // Returns the smallest rectangle that contains both the points (including
  // edges) of both aRect1 and aRect2.
  // Thus, empty input rectangles are allowed to affect the result.
  Sub UnionEdges(const Sub& aRect) const
  {
    Sub result;
    result.x = NS_MIN(x, aRect.x);
    result.y = NS_MIN(y, aRect.y);
    result.width = NS_MAX(XMost(), aRect.XMost()) - result.x;
    result.height = NS_MAX(YMost(), aRect.YMost()) - result.y;
    return result;
  }
  // Computes the smallest rectangle that contains both the area of both
  // aRect1 and aRect2, and fills 'this' with the result.
  // Thus, empty input rectangles are ignored.
  // If both rectangles are empty, sets 'this' to aRect2.
  //
  // 'this' can be the same object as either aRect1 or aRect2
  void UnionRect(const Sub& aRect1, const Sub& aRect2)
  {
    *static_cast<Sub*>(this) = aRect1.Union(aRect2);
  }

  // Computes the smallest rectangle that contains both the points (including
  // edges) of both aRect1 and aRect2.
  // Thus, empty input rectangles are allowed to affect the result.
  //
  // 'this' can be the same object as either aRect1 or aRect2
  void UnionRectEdges(const Sub& aRect1, const Sub& aRect2)
  {
    *static_cast<Sub*>(this) = aRect1.UnionEdges(aRect2);
  }

  void SetRect(T aX, T aY, T aWidth, T aHeight)
  {
    x = aX; y = aY; width = aWidth; height = aHeight;
  }
  void SetRect(const Point& aPt, const SizeT& aSize)
  {
    SetRect(aPt.x, aPt.y, aSize.width, aSize.height);
  }
  void MoveTo(T aX, T aY) { x = aX; y = aY; }
  void MoveTo(const Point& aPoint) { x = aPoint.x; y = aPoint.y; }
  void MoveBy(T aDx, T aDy) { x += aDx; y += aDy; }
  void MoveBy(const Point& aPoint) { x += aPoint.x; y += aPoint.y; }
  void SizeTo(T aWidth, T aHeight) { width = aWidth; height = aHeight; }
  void SizeTo(const SizeT& aSize) { width = aSize.width; height = aSize.height; }

  void Inflate(T aD) { Inflate(aD, aD); }
  void Inflate(T aDx, T aDy)
  {
    x -= aDx;
    y -= aDy;
    width += 2 * aDx;
    height += 2 * aDy;
  }
  void Inflate(const Margin& aMargin)
  {
    x -= aMargin.left;
    y -= aMargin.top;
    width += aMargin.LeftRight();
    height += aMargin.TopBottom();
  }
  void Inflate(const SizeT& aSize) { Inflate(aSize.width, aSize.height); }

  void Deflate(T aD) { Deflate(aD, aD); }
  void Deflate(T aDx, T aDy)
  {
    x += aDx;
    y += aDy;
    width = NS_MAX(T(0), width - 2 * aDx);
    height = NS_MAX(T(0), height - 2 * aDy);
  }
  void Deflate(const Margin& aMargin)
  {
    x += aMargin.left;
    y += aMargin.top;
    width = NS_MAX(T(0), width - aMargin.LeftRight());
    height = NS_MAX(T(0), height - aMargin.TopBottom());
  }
  void Deflate(const SizeT& aSize) { Deflate(aSize.width, aSize.height); }

  // Return true if the rectangles contain the same set of points, including
  // points on the edges.
  // Use when we care about the exact x/y/width/height values being
  // equal (i.e. we care about differences in empty rectangles).
  bool IsEqualEdges(const Sub& aRect) const
  {
    return x == aRect.x && y == aRect.y &&
           width == aRect.width && height == aRect.height;
  }
  // Return true if the rectangles contain the same area of the plane.
  // Use when we do not care about differences in empty rectangles.
  bool IsEqualInterior(const Sub& aRect) const
  {
    return IsEqualEdges(aRect) || (IsEmpty() && aRect.IsEmpty());
  }

  Sub operator+(const Point& aPoint) const
  {
    return Sub(x + aPoint.x, y + aPoint.y, width, height);
  }
  Sub operator-(const Point& aPoint) const
  {
    return Sub(x - aPoint.x, y - aPoint.y, width, height);
  }
  Sub& operator+=(const Point& aPoint)
  {
    MoveBy(aPoint);
    return *static_cast<Sub*>(this);
  }
  Sub& operator-=(const Point& aPoint)
  {
    MoveBy(-aPoint);
    return *static_cast<Sub*>(this);
  }

  // Find difference as a Margin
  Margin operator-(const Sub& aRect) const
  {
    return Margin(aRect.x - x, aRect.y - y,
                  XMost() - aRect.XMost(), YMost() - aRect.YMost());
  }

  // Helpers for accessing the vertices
  Point TopLeft() const { return Point(x, y); }
  Point TopRight() const { return Point(XMost(), y); }
  Point BottomLeft() const { return Point(x, YMost()); }
  Point BottomRight() const { return Point(XMost(), YMost()); }
  Point Center() const { return Point(x, y) + Point(width, height)/2; }
  SizeT Size() const { return SizeT(width, height); }

  // Helper methods for computing the extents
  T X() const { return x; }
  T Y() const { return y; }
  T Width() const { return width; }
  T Height() const { return height; }
  T XMost() const { return x + width; }
  T YMost() const { return y + height; }

  // Scale 'this' by aScale, converting coordinates to integers so that the result is
  // the smallest integer-coordinate rectangle containing the unrounded result.
  void ScaleRoundOut(double aScale) { ScaleRoundOut(aScale, aScale); }
  void ScaleRoundOut(double aXScale, double aYScale)
  {
    T right = static_cast<T>(NS_ceil(double(XMost()) * aXScale));
    T bottom = static_cast<T>(NS_ceil(double(YMost()) * aYScale));
    x = static_cast<T>(NS_floor(double(x) * aXScale));
    y = static_cast<T>(NS_floor(double(y) * aYScale));
    width = right - x;
    height = bottom - y;
  }
  void ScaleInverseRoundOut(double aScale) { ScaleInverseRoundOut(aScale, aScale); }
  void ScaleInverseRoundOut(double aXScale, double aYScale)
  {
    T right = static_cast<T>(ceil(double(XMost()) / aXScale));
    T bottom = static_cast<T>(ceil(double(YMost()) / aYScale));
    x = static_cast<T>(floor(double(x) / aXScale));
    y = static_cast<T>(floor(double(y) / aYScale));
    width = right - x;
    height = bottom - y;
  }

private:
  // Do not use the default operator== or operator!= !
  // Use IsEqualEdges or IsEqualInterior explicitly.
  bool operator==(const Sub& aRect) const { return false; }
  bool operator!=(const Sub& aRect) const { return false; }
};

}

#endif /* MOZILLA_BASERECT_H_ */
