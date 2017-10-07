// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// Copyright(C) 2013 Stephen McGranahan et al.
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
//--------------------------------------------------------------------------
//
// DESCRIPTION:
//      Linked portals
//      SoM created 02/13/06
//
//-----------------------------------------------------------------------------

#include "z_zone.h"

#include "c_io.h"
#include "doomstat.h"
#include "e_exdata.h"
#include "m_collection.h"  // ioanch 20160106
#include "p_chase.h"
#include "polyobj.h"
#include "p_portal.h"
#include "p_setup.h"
#include "p_user.h"
#include "r_main.h"
#include "r_portal.h"
#include "r_state.h"
#include "v_misc.h"

// SoM: Linked portals
// This list is allocated PU_LEVEL and is nullified in P_InitPortals. When the 
// table is built it is allocated as a linear buffer of groupcount * groupcount
// entries. The entries are arranged much like pixels in a screen buffer where 
// the offset is located in linktable[startgroup * groupcount + targetgroup]
linkoffset_t **linktable = NULL;


// This guy is a (0, 0, 0) link offset which is used to populate null links in the 
// linktable and is returned by P_GetLinkOffset for invalid inputs
linkoffset_t zerolink = {0, 0, 0};

// The group list is allocated PU_STATIC because it isn't level specific, however,
// each element is allocated PU_LEVEL. P_InitPortals clears the list and sets the 
// count back to 0
typedef struct 
{
   // List of sectors contained in the group
   sector_t **seclist;
   
   // Size of the list
   int listsize;
} pgroup_t;

static pgroup_t **groups = NULL;
static int      groupcount = 0;
static int      grouplimit = 0;

// This flag is a big deal. Heh, if this is true a whole lot of code will 
// operate differently. This flag is cleared on P_PortalInit and is ONLY to be
// set true by P_BuildLinkTable.
bool useportalgroups = false;

// ioanch 20160109: needed for sprite projecting
bool gMapHasSectorPortals;
bool gMapHasLinePortals;   // ioanch 20160131: needed for P_UseLines
bool *gGroupVisit;
// ioanch 20160227: each group may have a polyobject owner
const polyobj_t **gGroupPolyobject;

//
// P_PortalGroupCount
//
int P_PortalGroupCount()
{
   return useportalgroups ? groupcount : 1;
}

//
// P_InitPortals
//
// Called before map processing. Simply inits some module variables
void P_InitPortals(void)
{
   int i;
   linktable = NULL;

   groupcount = 0;
   for(i = 0; i < grouplimit; ++i)
      groups[i] = NULL;

   useportalgroups = false;
}

//
// R_SetSectorGroupID
//
// SoM: yes this is hackish, I admit :(
// This sets all mobjs inside the sector to have the sector id
// FIXME: why is this named R_ instead p_portal?
//
void R_SetSectorGroupID(sector_t *sector, int groupid)
{
   sector->groupid = groupid;

   // SoM: soundorg ids need to be set too
   sector->soundorg.groupid  = groupid;
   sector->csoundorg.groupid = groupid;

   // haleyjd 12/25/13: must scan thinker list, not use sector thinglist.
   for(auto th = thinkercap.next; th != &thinkercap; th = th->next)
   {
      Mobj *mo;
      if((mo = thinker_cast<Mobj *>(th)))
      {
         if(mo->subsector && mo->subsector->sector == sector)
            mo->groupid = groupid;
      }
   }

   // haleyjd 04/19/09: propagate to line sound origins
   for(int i = 0; i < sector->linecount; ++i)
      sector->lines[i]->soundorg.groupid = groupid;
}

//
// P_CreatePortalGroup
//
// This function creates a new portal group using the given sector as the 
// starting point for the group. P_GatherSectors is then called to gather 
// sectors into the group, and the newly created id is returned.
//
int P_CreatePortalGroup(sector_t *from)
{
   int       groupid = groupcount;
   pgroup_t  *group;
   
   if(from->groupid != R_NOGROUP)
      return from->groupid;
      
   if(groupcount == grouplimit)
   {
      grouplimit = grouplimit ? (grouplimit << 1) : 8;
      groups = erealloc(pgroup_t **, groups, sizeof(pgroup_t **) * grouplimit);
   }
   groupcount++;   
   
   
   group = groups[groupid] = (pgroup_t *)(Z_Malloc(sizeof(pgroup_t), PU_LEVEL, 0));
      
   group->seclist = NULL;
   group->listsize = 0;
  
   P_GatherSectors(from, groupid);
   return groupid;
}

//
// P_GatherSectors
//
// The function will run through the sector's lines list, and add 
// attached sectors to the group's sector list. As each sector is added the 
// currently forming group's id is assigned to that sector. This will continue
// until every attached sector has been added to the list, thus defining a 
// closed subspace of the map.
//
void P_GatherSectors(sector_t *from, int groupid)
{
   static sector_t   **list = NULL;
   static int        listmax = 0;

   sector_t  *sec2;
   pgroup_t  *group;
   line_t    *line;
   int       count = 0;
   int       i, sec, p;
   
   if(groupid < 0 || groupid >= groupcount)
   {
      // I have no idea if/when this would ever happen, but at any rate, it
      // would translate to EE itself doing something wrong.
      I_Error("P_GatherSectors: groupid invalid!");
   }

   group = groups[groupid];
   
   // Sector already has a group
   if(from->groupid != R_NOGROUP)
      return;

   // SoM: Just check for a null list here.
   if(!list || listmax <= numsectors)
   {
      listmax = numsectors + 1;
      list = erealloc(sector_t **, list, sizeof(sector_t *) * listmax);
   }

   R_SetSectorGroupID(from, groupid);
   list[count++] = from;
   
   for(sec = 0; sec < count; ++sec)
   {
      from = list[sec];

      for(i = 0; i < from->linecount; ++i)
      {
         // add any sectors to the list which aren't already there.
         line = from->lines[i];
         if((sec2 = line->frontsector))
         {
            for(p = 0; p < count; ++p)
            {
               if(sec2 == list[p])
                  break;
            }
            // if we didn't find the sector in the list, add it
            if(p == count)
            {
               list[count++] = sec2;
               R_SetSectorGroupID(sec2, groupid);
            }
         }

         if((sec2 = line->backsector))
         {
            for(p = 0; p < count; ++p)
            {
               if(sec2 == list[p])
                  break;
            }
            // if we didn't find the sector in the list, add it
            if(p == count)
            {
               list[count++] = sec2;
               R_SetSectorGroupID(sec2, groupid);
            }
         }
      }
   }

   // Ok, so expand the group list
   group->seclist = erealloc(sector_t **, group->seclist, 
                             sizeof(sector_t *) * (group->listsize + count));
   
   memcpy(group->seclist + group->listsize, list, count * sizeof(sector_t *));
   group->listsize += count;
}

//
// P_GetLinkOffset
//
// This function returns a linkoffset_t object which contains the map-space
// offset to get from the startgroup to the targetgroup. This will always return 
// a linkoffset_t object. In cases of invalid input or no link the offset will be
// (0, 0, 0)
//
linkoffset_t *P_GetLinkOffset(int startgroup, int targetgroup)
{
   if(!useportalgroups)
      return &zerolink;
      
   if(!linktable)
   {
      C_Printf(FC_ERROR "P_GetLinkOffset: called with no link table.\n");
      return &zerolink;
   }
   
   if(startgroup < 0 || startgroup >= groupcount)
   {
      C_Printf(FC_ERROR "P_GetLinkOffset: called with OoB start groupid %d.\n", startgroup);
      return &zerolink;
   }

   if(targetgroup < 0 || targetgroup >= groupcount)
   {
      C_Printf(FC_ERROR "P_GetLinkOffset: called with OoB target groupid %d.\n", targetgroup);
      return &zerolink;
   }

   auto link = linktable[startgroup * groupcount + targetgroup];
   return link ? link : &zerolink;
}

//
// P_GetLinkIfExists
//
// Returns a link offset to get from 'fromgroup' to 'togroup' if one exists. 
// Returns NULL otherwise
//
linkoffset_t *P_GetLinkIfExists(int fromgroup, int togroup)
{
   if(!useportalgroups)
      return NULL;

   if(!linktable)
   {
      C_Printf(FC_ERROR "P_GetLinkIfExists: called with no link table.\n");
      return NULL;
   }
   
   if(fromgroup < 0 || fromgroup >= groupcount)
   {
      C_Printf(FC_ERROR "P_GetLinkIfExists: called with OoB fromgroup %d.\n", fromgroup);
      return NULL;
   }

   if(togroup < 0 || togroup >= groupcount)
   {
      C_Printf(FC_ERROR "P_GetLinkIfExists: called with OoB togroup %d.\n", togroup);
      return NULL;
   }

   return linktable[fromgroup * groupcount + togroup];
}

//
// P_AddLinkOffset
//
// Returns 0 if the link offset was added successfully, 1 if the start group is
// out of bounds, and 2 of the target group is out of bounds.
//
static int P_AddLinkOffset(int startgroup, int targetgroup,
                           fixed_t x, fixed_t y, fixed_t z)
{
   linkoffset_t *link;

#ifdef RANGECHECK
   if(!linktable)
      I_Error("P_AddLinkOffset: no linktable allocated.\n");
#endif

   if(startgroup < 0 || startgroup >= groupcount)
      return 1; 
      //I_Error("P_AddLinkOffset: start groupid %d out of bounds.\n", startgroup);

   if(targetgroup < 0 || targetgroup >= groupcount)
      return 2; 
      //I_Error("P_AddLinkOffset: target groupid %d out of bounds.\n", targetgroup);

   if(startgroup == targetgroup)
      return 0;

   link = (linkoffset_t *)(Z_Malloc(sizeof(linkoffset_t), PU_LEVEL, 0));
   linktable[startgroup * groupcount + targetgroup] = link;
   
   link->x = x;
   link->y = y;
   link->z = z;

   return 0;
}

//
// P_CheckLinkedPortal
//
// This function performs various consistency and validation checks.
//
static bool P_CheckLinkedPortal(portal_t *portal, sector_t *sec)
{
   int i = eindex(sec - sectors);

   if(!portal || !sec)
      return true;
   if(portal->type != R_LINKED)
      return true;

   if(portal->data.link.toid == sec->groupid)
   {
      C_Printf(FC_ERROR "P_BuildLinkTable: sector %i portal references the "
               "portal group to which it belongs.\n"
               "Linked portals are disabled.\a\n", i);
      return false;
   }

   if(portal->data.link.fromid < 0 || 
      portal->data.link.fromid >= groupcount ||
      portal->data.link.toid < 0 || 
      portal->data.link.toid >= groupcount)
   {
      C_Printf(FC_ERROR "P_BuildLinkTable: sector %i portal has a groupid out "
               "of range.\nLinked portals are disabled.\a\n", i);
      return false;
   }

   if(sec->groupid < 0 || 
      sec->groupid >= groupcount)
   {
      C_Printf(FC_ERROR "P_BuildLinkTable: sector %i does not belong to a "
               "portal group.\nLinked portals are disabled.\a\n", i);
      return false;
   }
   
   if(sec->groupid != portal->data.link.fromid)
   {
      C_Printf(FC_ERROR "P_BuildLinkTable: sector %i does not belong to the "
               "the portal's fromid\nLinked portals are disabled.\a\n", i);
      return false;
   }

   auto link = linktable[sec->groupid * groupcount + portal->data.link.toid];

   // We've found a linked portal so add the entry to the table
   if(!link)
   {
      int ret = P_AddLinkOffset(sec->groupid, portal->data.link.toid,
                                portal->data.link.deltax, 
                                portal->data.link.deltay, 
                                portal->data.link.deltaz);
      if(ret)
         return false;
   }
   else
   {
      // Check for consistency
      if(link->x != portal->data.link.deltax ||
         link->y != portal->data.link.deltay ||
         link->z != portal->data.link.deltaz)
      {
         C_Printf(FC_ERROR "P_BuildLinkTable: sector %i in group %i contains "
                  "inconsistent reference to group %i.\n"
                  "Linked portals are disabled.\a\n", 
                  i, sec->groupid, portal->data.link.toid);
         return false;
      }
   }

   return true;
}

//
// P_GatherLinks
//
// This function generates linkoffset_t objects for every group to every other 
// group, that is, if group A has a link to B, and B has a link to C, a link
// can be found to go from A to C.
//
static void P_GatherLinks(int group, fixed_t dx, fixed_t dy, fixed_t dz,
                          int via)
{
   int i, p;
   linkoffset_t *link, **linklist, **grouplinks;

   // The main group has an indrect link with every group that links to a group
   // that has a direct link to it, or any group that has a link to a group the 
   // main group has an indirect link to. huh.

   // First step: run through the list of groups this group has direct links to
   // from there, run the function again with each direct link.
   if(via == R_NOGROUP)
   {
      linklist = linktable + group * groupcount;

      for(i = 0; i < groupcount; ++i)
      {
         if(i == group)
            continue;

         if((link = linklist[i]))
            P_GatherLinks(group, link->x, link->y, link->z, i);
      }

      return;
   }

   linklist = linktable + via * groupcount;
   grouplinks = linktable + group * groupcount;

   // Second step run through the linked group's link list. Ignore any groups 
   // the main group is already linked to. Add the deltas and add the entries,
   // then call this function for groups the linked group links to.
   for(p = 0; p < groupcount; ++p)
   {
      if(p == group || p == via)
         continue;

      if(!(link = linklist[p]) || grouplinks[p])
         continue;

      P_AddLinkOffset(group, p, dx + link->x, dy + link->y, dz + link->z);
      P_GatherLinks(group, dx + link->x, dy + link->y, dz + link->z, p);
   }
}

static void P_GlobalPortalStateCheck()
{
   sector_t *sec;
   line_t   *line;
   int      i;
   
   for(i = 0; i < numsectors; i++)
   {
      sec = sectors + i;
      
      if(sec->c_portal)
         P_CheckCPortalState(sec);
      if(sec->f_portal)
         P_CheckFPortalState(sec);
   }
   
   for(i = 0; i < numlines; i++)
   {
      line = lines + i;
      
      if(line->portal)
         P_CheckLPortalState(line);
   }
}

//
// P_buildPortalMap
//
// haleyjd 05/17/13: Build a blockmap-like array which will instantly tell
// whether or not a given blockmap cell contains linked portals of different
// types and may therefore need to be subject to differing clipping behaviors,
// such as disabling certain short circuit checks.
//
// ioanch 20151228: also flag for floor and ceiling portals
//
static void P_buildPortalMap()
{
   PODCollection<int> curGroups; // ioanch 20160106: keep list of current groups
   size_t pcount = P_PortalGroupCount();
   gGroupVisit = ecalloctag(bool *, sizeof(bool), pcount, PU_LEVEL, nullptr);
   // ioanch 20160227: prepare other groups too
   gGroupPolyobject = ecalloctag(decltype(gGroupPolyobject),
      sizeof(*gGroupPolyobject), pcount, PU_LEVEL, nullptr);
   pcount *= sizeof(bool);

   gMapHasSectorPortals = false; // init with false
   gMapHasLinePortals = false;
   
   auto addPortal = [&curGroups](int groupid)
   {
      if(!gGroupVisit[groupid])
      {
         gGroupVisit[groupid] = true;
         curGroups.add(groupid);
      }
   };
   
   int writeOfs;
   for(int y = 0; y < bmapheight; y++)
   {
      for(int x = 0; x < bmapwidth; x++)
      {
         int offset;
         int *list;
         
         curGroups.makeEmpty();
         memset(gGroupVisit, 0, pcount);

         writeOfs = offset = y * bmapwidth + x;
         offset = *(blockmap + offset);
         list = blockmaplump + offset;

         // skip 0 delimiter
         ++list;

         // ioanch: also check sector blockmaps
         int *tmplist = list;
         if(*tmplist == -1)   // empty blockmap
         {
            // ioanch: in case of no lines in blockmap, determine the sector
            // by looking in the centre
            const sector_t *sector = R_PointInSubsector(
               ::bmaporgx + (x << MAPBLOCKSHIFT) + (MAPBLOCKSHIFT / 2),
               ::bmaporgy + (y << MAPBLOCKSHIFT) + (MAPBLOCKSHIFT / 2))->sector;

            if(sector->c_pflags & PS_PASSABLE)
            {
               portalmap[writeOfs] |= PMF_CEILING;
               curGroups.add(sector->c_portal->data.link.toid);
               gMapHasSectorPortals = true;
            }
            if(sector->f_pflags & PS_PASSABLE)
            {
               portalmap[writeOfs] |= PMF_FLOOR;
               curGroups.add(sector->f_portal->data.link.toid);
               gMapHasSectorPortals = true;
            }
         }
         else for(; *tmplist != -1; tmplist++)
         {
            const line_t &li = lines[*tmplist];
            if(li.pflags & PS_PASSABLE)
            {
               portalmap[writeOfs] |= PMF_LINE;
               addPortal(li.portal->data.link.toid);
               gMapHasLinePortals = true;
            }
            if(li.frontsector->c_pflags & PS_PASSABLE)
            {
               portalmap[writeOfs] |= PMF_CEILING;
               addPortal(li.frontsector->c_portal->data.link.toid);
               gMapHasSectorPortals = true;
            }
            if(li.backsector && li.backsector->c_pflags & PS_PASSABLE)
            {
               portalmap[writeOfs] |= PMF_CEILING;
               addPortal(li.backsector->c_portal->data.link.toid);
               gMapHasSectorPortals = true;
            }
            if(li.frontsector->f_pflags & PS_PASSABLE)
            {
               portalmap[writeOfs] |= PMF_FLOOR;
               addPortal(li.frontsector->f_portal->data.link.toid);
               gMapHasSectorPortals = true;
            }
            if(li.backsector && li.backsector->f_pflags & PS_PASSABLE)
            {
               portalmap[writeOfs] |= PMF_FLOOR;
               addPortal(li.backsector->f_portal->data.link.toid);
               gMapHasSectorPortals = true;
            }
         }
         if(gBlockGroups[writeOfs])
            I_Error("P_buildPortalMap: non-null gBlockGroups entry!");
         
         size_t curSize = curGroups.getLength();
         gBlockGroups[writeOfs] = emalloctag(int *, 
            (curSize + 1) * sizeof(int), PU_LEVEL, nullptr);
         gBlockGroups[writeOfs][0] = static_cast<int>(curSize);
         // just copy...
         if(curSize)
         {
            memcpy(gBlockGroups[writeOfs] + 1, &curGroups[0], 
               curSize * sizeof(int));
         }
      }
   }

   pLPortalMap.mapInit();
}

//
// P_BuildLinkTable
//
bool P_BuildLinkTable()
{
   int i, p;
   sector_t *sec;
   linkoffset_t *link, *backlink;

   if(!groupcount)
      return true;

   // SoM: the last line of the table (starting at groupcount * groupcount) is
   // used as a temporary list for gathering links.
   linktable = 
      (linkoffset_t **)(Z_Calloc(1, sizeof(linkoffset_t *)*groupcount*groupcount,
                                PU_LEVEL, 0));

   // Run through the sectors check for invalid portal references.
   for(i = 0; i < numsectors; i++)
   {
      sec = sectors + i;
      
      // Make sure there are no groups that reference themselves or invalid group
      // id numbers.
      if(sec->groupid < R_NOGROUP || sec->groupid >= groupcount)
      {
         C_Printf(FC_ERROR "P_BuildLinkTable: sector %i has a groupid out of "
                  "range.\nLinked portals are disabled.\a\n", i);
         return false;
      }

      if(!P_CheckLinkedPortal(sec->c_portal, sec))
         return false;

      if(!P_CheckLinkedPortal(sec->f_portal, sec))
         return false;

      for(p = 0; p < sec->linecount; p++)
      {
         if(!P_CheckLinkedPortal(sec->lines[p]->portal, sec))
           return false;
      }
      // Sector checks out...
   }

   // Now the fun begins! Checking the actual groups for correct backlinks.
   // this needs to be done before the indirect link information is gathered to
   // make sure every link is two-way.
   for(i = 0; i < groupcount; i++)
   {
      for(p = 0; p < groupcount; p++)
      {
         if(p == i)
            continue;
         link = P_GetLinkOffset(i, p);
         backlink = P_GetLinkOffset(p, i);

         // check the links
         if(link && backlink)
         {
            if(backlink->x != -link->x ||
               backlink->y != -link->y ||
               backlink->z != -link->z)
            {
               C_Printf(FC_ERROR "Portal groups %i and %i link and backlink do "
                        "not agree\nLinked portals are disabled\a\n", i, p);
               return false;
            }
         }
      }
   }

   // That first loop has to complete before this can be run!
   for(i = 0; i < groupcount; i++)
      P_GatherLinks(i, 0, 0, 0, R_NOGROUP);

   // SoM: one last step. Find all map architecture with a group id of -1 and 
   // assign it to group 0
   for(i = 0; i < numsectors; i++)
   {
      if(sectors[i].groupid == R_NOGROUP)
         R_SetSectorGroupID(sectors + i, 0);
   }
   
   // Last step is to put zerolink in every link that goes from a group to that same group
   for(i = 0; i < groupcount; i++)
   {
      if(!linktable[i * groupcount + i])
         linktable[i * groupcount + i] = &zerolink;
   }

   // Everything checks out... let's run the portals
   useportalgroups = true;
   P_GlobalPortalStateCheck();

   // haleyjd 05/17/13: mark all blockmap cells where portals live.
   P_buildPortalMap();
   
   return true;
}

//
// P_LinkRejectTable
//
// Currently just clears each group for every other group.
//
void P_LinkRejectTable()
{
   int i, s, p, q;
   sector_t **list, **list2;

   for(i = 0; i < groupcount; i++)
   {
      list = groups[i]->seclist;
      for(s = 0; list[s]; s++)
      {
         int sectorindex1 = eindex(list[s] - sectors);

         for(p = 0; p < groupcount; p++)
         {
            if(i == p)
               continue;
            
            list2 = groups[p]->seclist;
            for(q = 0; list2[q]; q++)
            {
               int sectorindex2 = eindex(list2[q] - sectors);
               int pnum = (sectorindex1 * numsectors) + sectorindex2;

               rejectmatrix[pnum>>3] &= ~(1 << (pnum&7));
            } // q
         } // p
      } // s
   } // i
}

//
// The player passed a line portal from P_TryMove; just update viewport and
// pass-polyobject velocity
//
void P_LinePortalDidTeleport(Mobj *mo, fixed_t dx, fixed_t dy, fixed_t dz,
                             int fromid, int toid)
{
   // Prevent bad interpolation
   // FIXME: this is not interpolation, it's just instant movement; must be
   // fixed to be real interpolation even for the player (camera)
   mo->backupPosition();

   // Polyobject car enter and exit inertia
   const polyobj_t *poly[2] = { gGroupPolyobject[fromid],
      gGroupPolyobject[toid] };
   v2fixed_t pvel[2] = { };
   bool phave[2] = { };
   for(int i = 0; i < 2; ++i)
   {
      if(poly[i])
      {
         const auto th = thinker_cast<PolyMoveThinker *>(poly[i]->thinker);
         if(th)
         {
            pvel[i].x = th->momx;
            pvel[i].y = th->momy;
            phave[i] = true;
         }
         else
         {
            const auto th = thinker_cast<PolySlideDoorThinker *>(poly[i]->thinker);
            if(th && !th->delayCount)
            {
               pvel[i].x = th->momx;
               pvel[i].y = th->momy;
               phave[i] = true;
            }
            else
            {
               pvel[i].x = pvel[i].y = 0;
               phave[i] = false;
            }
         }
      }
   }
   if(phave[0] || phave[1])
   {
      // inertia!
      mo->momx += pvel[0].x - pvel[1].x;
      mo->momy += pvel[0].y - pvel[1].y;
   }

   // SoM: Boom's code for silent teleports. Fixes view bob jerk.
   // Adjust a player's view, in case there has been a height change
   if(mo->player)
   {
      // Save the current deltaviewheight, used in stepping
      fixed_t deltaviewheight = mo->player->deltaviewheight;

      // Clear deltaviewheight, since we don't want any changes now
      mo->player->deltaviewheight = 0;

      // Set player's view according to the newly set parameters
      P_CalcHeight(mo->player);

      mo->player->prevviewz = mo->player->viewz;

      // Reset the delta to have the same dynamics as before
      mo->player->deltaviewheight = deltaviewheight;

      if(mo->player == players + displayplayer)
         P_ResetChasecam();
   }

   //mo->backupPosition();
   P_AdjustFloorClip(mo);
}

// -----------------------------------------
// Begin portal teleportation
//
// EV_PortalTeleport
//
bool EV_PortalTeleport(Mobj *mo, fixed_t dx, fixed_t dy, fixed_t dz,
                       int fromid, int toid)
{
   if(!mo)
      return 0;

   // ioanch 20160113: don't teleport. Just change x and y
   P_UnsetThingPosition(mo);
   mo->x += dx;
   mo->y += dy;
   mo->z += dz;
   P_SetThingPosition(mo);

   P_LinePortalDidTeleport(mo, dx, dy, dz, fromid, toid);
   
   return 1;
}

//=============================================================================
//
// SoM: Utility functions
//

//
// P_GetPortalState
//
// Returns the combined state flags for the given portal based on various
// behavior flags
//
static int P_GetPortalState(const portal_t *portal, int sflags, bool obscured)
{
   bool active;
   int     ret = sflags & (PF_FLAGMASK | PS_OVERLAYFLAGS | PO_OPACITYMASK);
   
   if(!portal)
      return 0;
      
   active = !obscured && !(portal->flags & PF_DISABLED) && !(sflags & PF_DISABLED);
   
   if(active && !(portal->flags & PF_NORENDER) && !(sflags & PF_NORENDER))
      ret |= PS_VISIBLE;
      
   // Next two flags are for linked portals only
   active = (active && portal->type == R_LINKED && useportalgroups == true);
      
   if(active && !(portal->flags & PF_NOPASS) && !(sflags & PF_NOPASS))
      ret |= PS_PASSABLE;
   
   if(active && !(portal->flags & PF_BLOCKSOUND) && !(sflags & PF_BLOCKSOUND))
      ret |= PS_PASSSOUND;
      
   return ret;
}

void P_CheckCPortalState(sector_t *sec)
{
   bool     obscured;
   
   if(!sec->c_portal)
   {
      sec->c_pflags = 0;
      return;
   }
   
   obscured = (sec->c_portal->type == R_LINKED &&
               !(sec->c_pflags & PF_ATTACHEDPORTAL) &&
               sec->ceilingheight < sec->c_portal->data.link.planez);
               
   sec->c_pflags = P_GetPortalState(sec->c_portal, sec->c_pflags, obscured);
}

void P_CheckFPortalState(sector_t *sec)
{
   bool     obscured;
   
   if(!sec->f_portal)
   {
      sec->f_pflags = 0;
      return;
   }

   obscured = (sec->f_portal->type == R_LINKED &&
               !(sec->f_pflags & PF_ATTACHEDPORTAL) &&
               sec->floorheight > sec->f_portal->data.link.planez);
               
   sec->f_pflags = P_GetPortalState(sec->f_portal, sec->f_pflags, obscured);
}

void P_CheckLPortalState(line_t *line)
{
   if(!line->portal)
   {
      line->pflags = 0;
      return;
   }
   
   line->pflags = P_GetPortalState(line->portal, line->pflags, false);
}

//
// P_SetFloorHeight
//
// This function will set the floor height, and update
// the float version of the floor height as well.
//
void P_SetFloorHeight(sector_t *sec, fixed_t h)
{
   // set new value
   sec->floorheight = h;
   sec->floorheightf = M_FixedToFloat(sec->floorheight);

   // check floor portal state
   P_CheckFPortalState(sec);
}

//
// P_SetCeilingHeight
//
// This function will set the ceiling height, and update
// the float version of the ceiling height as well.
//
void P_SetCeilingHeight(sector_t *sec, fixed_t h)
{
   // set new value
   sec->ceilingheight = h;
   sec->ceilingheightf = M_FixedToFloat(sec->ceilingheight);

   // check ceiling portal state
   P_CheckCPortalState(sec);
}

void P_SetPortalBehavior(portal_t *portal, int newbehavior)
{
   int   i;
   
   portal->flags = newbehavior & PF_FLAGMASK;
   for(i = 0; i < numsectors; i++)
   {
      sector_t *sec = sectors + i;
      
      if(sec->c_portal == portal)
         P_CheckCPortalState(sec);
      if(sec->f_portal == portal)
         P_CheckFPortalState(sec);
   }
   
   for(i = 0; i < numlines; i++)
   {
      if(lines[i].portal == portal)
         P_CheckLPortalState(lines + i);
   }
}

void P_SetFPortalBehavior(sector_t *sec, int newbehavior)
{
   if(!sec->f_portal)
      return;
      
   sec->f_pflags = newbehavior;
   P_CheckFPortalState(sec);
}

void P_SetCPortalBehavior(sector_t *sec, int newbehavior)
{
   if(!sec->c_portal)
      return;
      
   sec->c_pflags = newbehavior;
   P_CheckCPortalState(sec);
}

void P_SetLPortalBehavior(line_t *line, int newbehavior)
{
   if(!line->portal)
      return;
      
   line->pflags = newbehavior;
   P_CheckLPortalState(line);
}

//
// P_MoveLinkedPortal
//
// ioanch 20160226: moves the offset of a linked portal
// TODO: do the same for anchored portals
//
void P_MoveLinkedPortal(portal_t *portal, fixed_t dx, fixed_t dy, bool movebehind)
{
   linkdata_t &data = portal->data.link;
   data.deltax += dx;
   data.deltay += dy;
   linkoffset_t *link;
   for(int i = 0; i < groupcount; ++i)
   {
      if(movebehind)
      {
         // the group behind appears to be moving in relation to the others
         link = P_GetLinkOffset(i, data.toid);
         if(link == &zerolink)
            continue;
         link->x += dx;
         link->y += dy;
      }
      else
      {
         // the group in front of the portal appears to be moving
         link = P_GetLinkOffset(data.fromid, i);
         if(link == &zerolink)
            continue;
         link->x += dx;
         link->y += dy;
      }
   }
}

//
// Returns ceiling portal Z, which depends on whether planez is used or not.
// Assumes linked portal exists and is active.
//
fixed_t P_CeilingPortalZ(const sector_t &sector)
{
   return sector.c_pflags & PF_ATTACHEDPORTAL ? sector.ceilingheight :
   sector.c_portal->data.link.planez;
}

//
// Same but for floors
//
fixed_t P_FloorPortalZ(const sector_t &sector)
{
   return sector.f_pflags & PF_ATTACHEDPORTAL ? sector.floorheight :
   sector.f_portal->data.link.planez;
}

//
// P_BlockHasLinkedPortalLines
//
// ioanch 20160228: return true if block has portalmap 1 or a polyportal
// It's coarse
//
bool P_BlockHasLinkedPortals(int index, bool includesectors)
{
   // safe for overflow
   if(index < 0 || index >= bmapheight * bmapwidth)
      return false;
   if(portalmap[index] & (PMF_LINE |
      (includesectors ? PMF_FLOOR | PMF_CEILING : 0)))
   {
      return true;
   }
   
   for(const DLListItem<polymaplink_t> *plink = polyblocklinks[index]; plink;
      plink = plink->dllNext)
   {
      if((*plink)->po->hasLinkedPortals)
         return true;
   }
   return false;
}

//==============================================================================
//
// More portal blockmap stuff (besides portalmap and gBlockGroups from p_setup)
//
LinePortalBlockmap pLPortalMap;

//
// Initialization per map. Makes PU_LEVEL allocations
//
void LinePortalBlockmap::mapInit()
{
   mMap.clear();
   int numblocks = bmapwidth * bmapheight;
   for(int i = 0; i < numblocks; ++i)
   {
      int offset = blockmap[i];
      const int *list = blockmaplump + offset;
      // MaxW: 2016/02/02: if before 3.42 always skip, skip if all blocklists start w/ 0
      // killough 2/22/98: demo_compatibility check
      // skip 0 starting delimiter -- phares
      if((!demo_compatibility && demo_version < 342) ||
         (demo_version >= 342 && skipblstart))
      {
         list++;
      }

      PODCollection<const line_t *> coll;

      for(; *list != -1; ++list)
      {
         if(*list >= numlines)
            continue;
         const line_t &line = lines[*list];
         if(line.backsector &&
            (line.pflags & PS_PASSABLE ||
             (line.extflags & EX_ML_LOWERPORTAL &&
              line.backsector->f_portal &&
              line.backsector->f_portal->type == R_LINKED) ||
             (line.extflags & EX_ML_UPPERPORTAL &&
              line.backsector->c_portal &&
              line.backsector->c_portal->type == R_LINKED)))
         {
            coll.add(&line);
         }
      }

      mMap.add(coll);
   }

   mValids = ecalloctag(decltype(mValids), numlines, sizeof(*mValids), PU_LEVEL,
                        nullptr);
}

//
// Does the iteration
//
bool LinePortalBlockmap::iterator(int x, int y, void *data,
                                  bool (*func)(const line_t &, void *data)) const
{
   if(x < 0 || x >= bmapwidth || y < 0 || y >= bmapheight)
      return true;
   int i = y * bmapwidth + x;
   const PODCollection<const line_t *> coll = mMap[i];
   for(const line_t *line : coll)
   {
      if(mValids[line - lines] == mValidcount)
         continue;
      mValids[line - lines] = mValidcount;
      if(!func(*line, data))
         return false;
   }
   return true;
}

// EOF

