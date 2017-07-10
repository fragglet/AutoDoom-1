// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// Copyright(C) 2013 Ioan Chera
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
// Additional terms and conditions compatible with the GPLv3 apply. See the
// file COPYING-EE for details.
//
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//      Temporary bot map, processed to result into the final bot map
//
//-----------------------------------------------------------------------------

#ifndef B_BOTMAPTEMP_H_
#define B_BOTMAPTEMP_H_

#include <unordered_set>
#include <map>
#include <set>

#include "b_intset.h"
#include "b_msector.h"
#include "../m_collection.h"
#include "../m_dllist.h"
#include "../m_fixed.h"

//typedef std::unordered_set<int> IntSet;
//typedef std::set<int> IntOSet;

class OutBuffer;

//
// TempBotMap
//
// Temporary bot map, with intermediary data. NOTE: some of its data is already
// placed in BotMap
//
class TempBotMapPImpl;
class TempBotMap : public ZoneObject
{
   TempBotMapPImpl *pimpl;
   friend class TempBotMapPImpl;
public:
   //
   // Vertex
   //
   // A vertex of the map
   //
//   struct Vertex
//   {
//      DLListItem<Vertex> listLink;  // link within global list
//      DLListItemNC<Vertex> blockLink; // link within map block
//      fixed_t x, y;                 // coordinates
//      int blockIndex;
//      int degree;                      // graph degree
//   };

   //
   // Line
   //
   // A line of the map
   //
   class Line : public ZoneObject
   {
   public:
      DLListItemNC<Line> listLink;
//      Vertex *v1, *v2;        // end points
      v2fixed_t v1, v2;          // coordinates
      PODCollection<int> blockIndices; // blockmap links
      IntOSet msecIndices[2];  // metasector links
      MetaSector *metasec[2];
      const line_t* assocLine;
   };
   typedef std::unordered_set<Line *> LinePtrSet;
private:
   
   //
   // MEMBER VARIABLES
   //
   
   bool generated;   // initialization flag
   fixed_t radius;   // reduction radius
   
//   DLList<Vertex, &Vertex::listLink> vertexList;
//   DLListNC<Vertex, &Vertex::blockLink> *vertexBMap;
   DLListNC<Line, &Line::listLink> lineList;
   Collection<LinePtrSet > lineBMap;
   DLList<MetaSector, &MetaSector::listLink> msecList;
   

   //
   // PRIVATE METHODS
   //
   
   void createBlockMap();
   
//   void deleteVertex(Vertex *vert);
   void deleteLine(Line *ln, IntOSet *targfront, IntOSet *targback);
   Line &placeLine(v2fixed_t v1, v2fixed_t v2, const line_t* assocLine = nullptr,
                   const IntOSet *msecGen = nullptr,
                   const IntOSet *bsecGen = nullptr);
   
   void obtainMetaSectors();
   
   void clearRedundantLines();
   void clearUnusedVertices();
   
public:
	DLList<MetaSector, &MetaSector::listLink> &getMsecList()
	{
		return msecList;
	}
	
   v2fixed_t placeVertex(v2fixed_t v);

   struct vectless
   {
      bool operator()(v2fixed_t v1, v2fixed_t v2) const
      {
         return v1.x < v2.x ? true : v2.x < v1.x ? false : v1.y < v2.y;
      }
   };

   std::map<v2fixed_t, int, vectless> vertexMap;
   PODCollection<v2fixed_t> vertexList;

   TempBotMap();
   ~TempBotMap();
   void generateForRadius(fixed_t inradius);
   const DLListItemNC<Line> *lineGet() const {return lineList.head;}
//   const DLListItem<Vertex> *vertGet() const {return vertexList.head;}
   const DLListItem<MetaSector> *msecGet() const {return msecList.head;}
   template <typename T> void setItemIndex(int dat, T *obj)
   {
      obj->listLink.dllData = dat;
   }
};
extern TempBotMap *tempBotMap;
// pointer to the map. Will be created dynamically with
                        // a PU_LEVEL tag

#endif
// EOF

