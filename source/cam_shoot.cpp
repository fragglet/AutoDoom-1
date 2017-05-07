//
// The Eternity Engine
// Copyright(C) 2016 James Haley, Ioan Chera, et al.
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
// Purpose: linked portal aware shooting
// Authors: Ioan Chera
//

#include "z_zone.h"

#include "cam_common.h"
#include "cam_sight.h"
#include "e_exdata.h"
#include "p_inter.h"
#include "p_mobj.h"
#include "p_pspr.h"
#include "p_spec.h"
#include "r_defs.h"
#include "r_main.h"
#include "r_pcheck.h"
#include "r_portal.h"
#include "r_sky.h"

#define RECURSION_LIMIT 64

//
// For bullet attacks
//
class ShootContext
{
public:
   struct State
   {
      const ShootContext *prev;
      fixed_t x, y, z;
      fixed_t origindist;
      int groupid;
      int reclevel;
   };

   static void lineAttack(Mobj *source, angle_t angle, fixed_t distance,
      fixed_t slope, int damage, const State *state);
private:

   bool checkShootFlatPortal(const sector_t *sector, fixed_t infrac) const;
   bool shoot2SLine(line_t *li, int lineside, fixed_t dist,
      const lineopening_t &lo) const;
   bool shotCheck2SLine(line_t *li, int lineside, fixed_t dist) const;
   static bool shootTraverse(const intercept_t *in, void *data,
      const divline_t &trace);
   ShootContext(Mobj *source, angle_t inangle, fixed_t distance, fixed_t slope,
      int indamage, const State *instate);

   Mobj *thing;
   angle_t angle;
   int damage;
   fixed_t attackrange;
   fixed_t aimslope;
   fixed_t cos, sin;
   State state;
};

//
// Caller
//
void ShootContext::lineAttack(Mobj *source, angle_t angle, fixed_t distance,
   fixed_t slope, int damage, const State *state)
{
   ShootContext context(source, angle, distance, slope, damage, state);
   fixed_t x2 = context.state.x + (distance >> FRACBITS) * context.cos;
   fixed_t y2 = context.state.y + (distance >> FRACBITS) * context.sin;

   PTDef def;
   def.flags = CAM_ADDLINES | CAM_ADDTHINGS;
   def.earlyOut = PTDef::eo_no;
   def.trav = shootTraverse;
   PathTraverser traverser(def, &context);

   if(traverser.traverse(context.state.x, context.state.y, x2, y2))
   {
      // if 100% passed, check if the final sector was crossed
      const sector_t *endsector = R_PointInSubsector(x2, y2)->sector;
      context.checkShootFlatPortal(endsector, FRACUNIT);
   }
}

//
// Check if hit a portal
//
bool ShootContext::checkShootFlatPortal(const sector_t *sidesector,
   fixed_t infrac) const
{
   const linkdata_t *portaldata = nullptr;
   fixed_t pfrac = 0;
   fixed_t absratio = 0;
   int newfromid = R_NOGROUP;
   fixed_t z = state.z + FixedMul(aimslope, FixedMul(infrac, attackrange));

   if(sidesector->c_pflags & PS_PASSABLE)
   {
      // ceiling portal
      fixed_t planez = R_CPLink(sidesector)->planez;
      if(z > planez)
      {
         pfrac = FixedDiv(planez - state.z, aimslope);
         absratio = FixedDiv(planez - state.z, z - state.z);
         z = planez;
         portaldata = R_CPLink(sidesector);
         newfromid = sidesector->c_portal->data.link.toid;
      }
   }
   if(!portaldata && sidesector->f_pflags & PS_PASSABLE)
   {
      // floor portal
      fixed_t planez = R_FPLink(sidesector)->planez;
      if(z < planez)
      {
         pfrac = FixedDiv(planez - state.z, aimslope);
         absratio = FixedDiv(planez - state.z, z - state.z);
         z = planez;
         portaldata = R_FPLink(sidesector);
         newfromid = sidesector->f_portal->data.link.toid;
      }
   }
   if(portaldata && pfrac > 0)
   {
      // update x and y as well
      fixed_t x = state.x + FixedMul(cos, pfrac);
      fixed_t y = state.y + FixedMul(sin, pfrac);
      if(newfromid == state.groupid || state.reclevel >= RECURSION_LIMIT)
         return false;

      // NOTE: for line attacks, sightzstart also moves!
      fixed_t dist = FixedMul(FixedMul(attackrange, infrac), absratio);
      fixed_t remdist = attackrange - dist;

      x += portaldata->deltax;
      y += portaldata->deltay;
      z += portaldata->deltaz;

      State newstate(state);
      newstate.groupid = newfromid;
      newstate.origindist += dist;
      newstate.prev = this;
      newstate.x = x;
      newstate.y = y;
      newstate.z = z;
      newstate.reclevel++;

      lineAttack(thing, angle, remdist, aimslope, damage, &newstate);

      return true;
   }
   return false;
}

//
// ShootContext::shoot2SLine
//
bool ShootContext::shoot2SLine(line_t *li, int lineside, fixed_t dist,
   const lineopening_t &lo) const
{
   // ioanch: no more need for demo version < 333 check. Also don't allow comp.
   if(FixedDiv(lo.openbottom - state.z, dist) <= aimslope &&
      FixedDiv(lo.opentop - state.z, dist) >= aimslope)
   {
      if(li->special)
         P_ShootSpecialLine(thing, li, lineside);
      return true;
   }
   return false;
}

//
// ShootContext::shotCheck2SLine
//
bool ShootContext::shotCheck2SLine(line_t *li, int lineside, fixed_t dist) const
{
   bool ret = false;
   if(li->extflags & EX_ML_BLOCKALL)
      return false;

   if(li->flags & ML_TWOSIDED)
   {
      lineopening_t lo = { 0 };
      lo.calculate(li);
      if(shoot2SLine(li, lineside, dist, lo))
         ret = true;
   }
   return ret;
}

//
// ShootContext::shootTraverse
//
bool ShootContext::shootTraverse(const intercept_t *in, void *data,
   const divline_t &trace)
{
   auto &context = *static_cast<ShootContext *>(data);
   if(in->isaline)
   {
      line_t *li = in->d.line;

      // ioanch 20160101: use the trace origin instead of assuming it being the
      // same as the source thing origin, as it happens in PTR_ShootTraverse
      int lineside = P_PointOnLineSide(trace.x, trace.y, li);

      fixed_t dist = FixedMul(context.attackrange, in->frac);
      if(context.shotCheck2SLine(li, lineside, dist))
      {
         // ioanch 20160101: line portal aware
         const portal_t *portal = nullptr;
         if(li->extflags & EX_ML_LOWERPORTAL && li->backsector &&
            li->backsector->f_pflags & PS_PASSABLE &&
            FixedDiv(li->backsector->floorheight - context.state.z, dist)
            >= context.aimslope)
         {
            portal = li->backsector->f_portal;
         }
         else if(li->extflags & EX_ML_UPPERPORTAL && li->backsector &&
            li->backsector->c_pflags & PS_PASSABLE &&
            FixedDiv(li->backsector->ceilingheight - context.state.z, dist)
            <= context.aimslope)
         {
            portal = li->backsector->c_portal;
         }
         else if(li->pflags & PS_PASSABLE &&
            (!(li->extflags & EX_ML_LOWERPORTAL) ||
               FixedDiv(li->backsector->floorheight - context.state.z, dist)
               < context.aimslope))
         {
            portal = li->portal;
         }
         if(portal && lineside == 0 && in->frac > 0)
         {
            int newfromid = portal->data.link.toid;
            if(newfromid == context.state.groupid)
               return true;

            if(context.state.reclevel >= RECURSION_LIMIT)
               return true;

            // NOTE: for line attacks, sightzstart also moves!
            fixed_t x = trace.x + FixedMul(trace.dx, in->frac);
            fixed_t y = trace.y + FixedMul(trace.dy, in->frac);
            fixed_t z = context.state.z + FixedMul(context.aimslope,
               FixedMul(in->frac, context.attackrange));
            fixed_t dist = FixedMul(context.attackrange, in->frac);
            fixed_t remdist = context.attackrange - dist;

            const linkdata_t &data = portal->data.link;

            x += data.deltax;
            y += data.deltay;
            z += data.deltaz;  // why not

            State newstate(context.state);
            newstate.groupid = newfromid;
            newstate.x = x;
            newstate.y = y;
            newstate.z = z;
            newstate.prev = &context;
            newstate.origindist += dist;
            ++newstate.reclevel;

            lineAttack(context.thing, context.angle, remdist, context.aimslope,
               context.damage, &newstate);

            return false;
         }

         return true;
      }

      // ioanch 20160102: compensate the current range with the added one
      fixed_t frac = in->frac - FixedDiv(4 * FRACUNIT, context.attackrange +
         context.state.origindist);
      fixed_t x = trace.x + FixedMul(trace.dx, frac);
      fixed_t y = trace.y + FixedMul(trace.dy, frac);
      fixed_t z = context.state.z + FixedMul(context.aimslope,
         FixedMul(frac, context.attackrange));

      const sector_t *sidesector = lineside ? li->backsector : li->frontsector;
      bool hitplane = false;
      int updown = 2;

      // ioanch 20160102: check for Z portals

      if(sidesector)
      {
         if(context.checkShootFlatPortal(sidesector, in->frac))
            return false;  // done here

         if(z < sidesector->floorheight)
         {
            fixed_t pfrac = FixedDiv(sidesector->floorheight
               - context.state.z, context.aimslope);

            if(R_IsSkyFlat(sidesector->floorpic) ||
               R_IsSkyLikePortalFloor(*sidesector))
            {
               return false;
            }

            x = trace.x + FixedMul(context.cos, pfrac);
            y = trace.y + FixedMul(context.sin, pfrac);
            z = sidesector->floorheight;

            hitplane = true;
            updown = 0;
         }
         else if(z > sidesector->ceilingheight)
         {
            fixed_t pfrac = FixedDiv(sidesector->ceilingheight
               - context.state.z, context.aimslope);
            if(sidesector->intflags & SIF_SKY ||
               R_IsSkyLikePortalCeiling(*sidesector))
            {
               return false;
            }
            x = trace.x + FixedMul(context.cos, pfrac);
            y = trace.y + FixedMul(context.sin, pfrac);
            z = sidesector->ceilingheight;

            hitplane = true;
            updown = 1;
         }
      }

      if(!hitplane && li->special)
         P_ShootSpecialLine(context.thing, li, lineside);

      if(R_IsSkyFlat(li->frontsector->ceilingpic) || li->frontsector->c_portal)
      {
         if(z > li->frontsector->ceilingheight)
            return false;
         if(li->backsector && R_IsSkyFlat(li->backsector->ceilingpic))
         {
            if(li->backsector->ceilingheight < z)
               return false;
         }
      }

      if(demo_version >= 342 && li->backsector &&
         ((li->extflags & EX_ML_UPPERPORTAL &&
            li->backsector->ceilingheight < li->frontsector->ceilingheight &&
            li->backsector->ceilingheight < z &&
            R_IsSkyLikePortalCeiling(*li->backsector)) ||
            (li->extflags & EX_ML_LOWERPORTAL &&
               li->backsector->floorheight > li->frontsector->floorheight &&
               li->backsector->floorheight > z &&
               R_IsSkyLikePortalFloor(*li->backsector))))
      {
         return false;
      }

      if(!hitplane && !li->backsector && R_IsSkyLikePortalWall(*li))
         return false;

      P_SpawnPuff(x, y, z, P_PointToAngle(0, 0, li->dx, li->dy) - ANG90,
         updown, true);

      return false;
   }
   else
   {
      Mobj *th = in->d.thing;
      // self and shootable checks already handled. Friendliness check not
      // enabled anyway
      if(!(th->flags & MF_SHOOTABLE) || th == context.thing)
         return true;

      if(th->flags3 & MF3_GHOST && context.thing->player
         && P_GetReadyWeapon(context.thing->player)->flags & WPF_NOHITGHOSTS)
      {
         return true;
      }

      fixed_t dist = FixedMul(context.attackrange, in->frac);
      fixed_t thingtopslope = FixedDiv(th->z + th->height - context.state.z,
         dist);
      fixed_t thingbottomslope = FixedDiv(th->z - context.state.z, dist);

      if(thingtopslope < context.aimslope)
         return true;

      if(thingbottomslope > context.aimslope)
         return true;

      // ioanch 20160102: compensate
      fixed_t frac = in->frac - FixedDiv(10 * FRACUNIT, context.attackrange +
         context.state.origindist);
      fixed_t x = trace.x + FixedMul(trace.dx, frac);
      fixed_t y = trace.y + FixedMul(trace.dy, frac);
      fixed_t z = context.state.z + FixedMul(context.aimslope, FixedMul(frac,
         context.attackrange));

      if(th->flags & MF_NOBLOOD || th->flags2 & (MF2_INVULNERABLE | MF2_DORMANT))
      {
         P_SpawnPuff(x, y, z, P_PointToAngle(0, 0, trace.dx, trace.dy)
            - ANG180, 2, true);
      }
      else
      {
         BloodSpawner(th, x, y, z, context.damage, trace, context.thing).spawn(BLOOD_SHOT);
      }
      if(context.damage)
      {
         P_DamageMobj(th, context.thing, context.thing, context.damage,
            context.thing->info->mod);
      }

      return false;
   }
}

//
// ShootContext::ShootContext
//
ShootContext::ShootContext(Mobj *source, angle_t inangle, fixed_t distance,
   fixed_t slope, int indamage,
   const State *instate) :
   thing(source), angle(inangle), damage(indamage), attackrange(distance),
   aimslope(slope)
{
   inangle >>= ANGLETOFINESHIFT;
   cos = finecosine[inangle];
   sin = finesine[inangle];
   if(instate)
      state = *instate;
   else
   {
      state.x = source->x;
      state.y = source->y;
      state.z = source->z - source->floorclip + (source->height >> 1) +
         8 * FRACUNIT;
      state.groupid = source->groupid;
      state.prev = nullptr;
      state.origindist = 0;
      state.reclevel = 0;
   }
}

//
// CAM_LineAttack
//
// Portal-aware bullet attack
//
void CAM_LineAttack(Mobj *source, angle_t angle, fixed_t distance,
   fixed_t slope, int damage)
{
   ShootContext::lineAttack(source, angle, distance, slope, damage, nullptr);
}



// EOF

