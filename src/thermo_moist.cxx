/*
 * MicroHH
 * Copyright (c) 2011-2013 Chiel van Heerwaarden
 * Copyright (c) 2011-2013 Thijs Heus
 *
 * This file is part of MicroHH
 *
 * MicroHH is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * MicroHH is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with MicroHH.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <sstream>
#include <algorithm>
#include "grid.h"
#include "fields.h"
#include "thermo_moist.h"
#include "diff_les2s.h"
#include "defines.h"
#include "model.h"
#include "stats.h"
#include "master.h"
#include "cross.h"

#define rd 287.04
#define rv 461.5
#define ep rd/rv
#define cp 1005
#define lv 2.5e6
#define rhow    1.e3
#define tmelt   273.15
#define p0 1.e5
#define grav 9.81

#define ex1 2.85611940298507510698e-06
#define ex2 -1.02018879928714644313e-11
#define ex3 5.82999832046362073082e-17
#define ex4 -3.95621945728655163954e-22
#define ex5 2.93898686274077761686e-27
#define ex6 -2.30925409555411170635e-32
#define ex7 1.88513914720731231360e-37

#define at 17.27
#define bt 35.86
#define es0 610.78

#define c0 0.6105851e+03
#define c1 0.4440316e+02
#define c2 0.1430341e+01
#define c3 0.2641412e-01
#define c4 0.2995057e-03
#define c5 0.2031998e-05
#define c6 0.6936113e-08
#define c7 0.2564861e-11
#define c8 -.3704404e-13

#define NO_OFFSET 0.

cthermo_moist::cthermo_moist(cmodel *modelin) : cthermo(modelin)
{
  swthermo = "moist";
  allocated = false;
}

cthermo_moist::~cthermo_moist()
{
  if(allocated)
  {
    delete[] thl0;
    delete[] qt0;
    delete[] thvref;
    delete[] thvrefh;
    delete[] exnref;
    delete[] exnrefh;
    delete[] pref;
    delete[] prefh;
  }
}

int cthermo_moist::readinifile(cinput *inputin)
{
  int nerror = 0;
  nerror += inputin->getItem(&ps    , "thermo", "ps"    , "");

  nerror += fields->initpfld("s", "Liquid water potential temperature", "K");
  nerror += inputin->getItem(&fields->sp["s"]->visc, "fields", "svisc", "s");
  nerror += fields->initpfld("qt", "Total water mixing ratio", "kg kg-1");
  nerror += inputin->getItem(&fields->sp["qt"]->visc, "fields", "svisc", "qt");

  // Read list of cross sections
  nerror += inputin->getList(&crosslist , "thermo", "crosslist" , "");

  // BvS test for updating hydrostatic prssure during run
  // swupdate..=0 -> initial base state pressure used in saturation calc
  // swupdate..=1 -> base state pressure updated before saturation calc
  nerror += inputin->getItem(&swupdatebasestate,"thermo","swupdatebasestate","");

  return (nerror > 0);
}

int cthermo_moist::init()
{
  stats = model->stats;

  thl0    = new double[grid->kcells];
  qt0     = new double[grid->kcells];
  thvref  = new double[grid->kcells];
  thvrefh = new double[grid->kcells];
  exnref  = new double[grid->kcells];
  exnrefh = new double[grid->kcells];
  pref    = new double[grid->kcells];
  prefh   = new double[grid->kcells];

  return 0;
}

int cthermo_moist::create(cinput *inputin)
{
  int kstart = grid->kstart;
  int kend   = grid->kend;
  int nerror = 0;

  // Enable automated calculation of horizontally averaged fields
  fields->setcalcprofs(true);

  allocated = true;

  // Calculate the base state profiles. With swupdatebasestate=1, these profiles 
  // are updated on every tstep. First take the initial profile as the reference
  if(inputin->getProf(&thl0[grid->kstart], "s", grid->kmax))
    return 1;
  if(inputin->getProf(&qt0[grid->kstart], "qt", grid->kmax))
    return 1;

  // Calculate surface and model top values thl and qt
  double thl0s, qt0s, thl0t, qt0t;
  thl0s = thl0[kstart] - grid->z[kstart]*(thl0[kstart+1]-thl0[kstart])*grid->dzhi[kstart+1];
  qt0s  = qt0[kstart]  - grid->z[kstart]*(qt0[kstart+1] -qt0[kstart] )*grid->dzhi[kstart+1];
  thl0t = thl0[kend-1] + (grid->zh[kend]-grid->z[kend-1])*(thl0[kend-1]-thl0[kend-2])*grid->dzhi[kend-1];
  qt0t  = qt0[kend-1]  + (grid->zh[kend]-grid->z[kend-1])*(qt0[kend-1]- qt0[kend-2] )*grid->dzhi[kend-1];

  // Set the ghost cells for the reference temperature and moisture
  thl0[kstart-1]  = 2.*thl0s - thl0[kstart];
  thl0[kend]      = 2.*thl0t - thl0[kend-1];
  qt0[kstart-1]   = 2.*qt0s  - qt0[kstart];
  qt0[kend]       = 2.*qt0t  - qt0[kend-1];

  // Calculate the initial/reference base state
  calcbasestate(pref, prefh, fields->rhoref, fields->rhorefh, thvref, thvrefh, exnref, exnrefh, thl0, qt0);

  // add variables to the statistics
  if(stats->getsw() == "1")
  {
    // Add base state pressure and density to statistics
    stats->addfixedprof("pref",    "Full level basic state pressure", "Pa",     "z",  pref);
    stats->addfixedprof("prefh",   "Half level basic state pressure", "Pa",     "zh", prefh);
    stats->addfixedprof("rhoref",  "Full level basic state density",  "kg m-3", "z",  fields->rhoref);
    stats->addfixedprof("rhorefh", "Half level basic state density",  "kg m-3", "zh", fields->rhorefh);

    stats->addprof("b", "Buoyancy", "m s-2", "z");
    for(int n=2; n<5; ++n)
    {
      std::stringstream ss;
      ss << n;
      std::string sn = ss.str();
      stats->addprof("b"+sn, "Moment " +sn+" of the buoyancy", "(m s-2)"+sn,"z");
    }

    stats->addprof("bgrad", "Gradient of the buoyancy", "m s-3", "zh");
    stats->addprof("bw"   , "Turbulent flux of the buoyancy", "m2 s-3", "zh");
    stats->addprof("bdiff", "Diffusive flux of the buoyancy", "m2 s-3", "zh");
    stats->addprof("bflux", "Total flux of the buoyancy", "m2 s-3", "zh");

    stats->addprof("ql", "Liquid water mixing ratio", "kg kg-1", "z");
    stats->addprof("cfrac", "Cloud fraction", "-","z");

    stats->addtseries("lwp", "Liquid water path", "kg m-2");
    stats->addtseries("ccover", "Projected cloud cover", "-");
  }

  // Cross sections (isn't there an easier way to populate this list?)
  allowedcrossvars.push_back("b");
  allowedcrossvars.push_back("bbot");
  allowedcrossvars.push_back("bfluxbot");
  if(grid->swspatialorder == "4")
    allowedcrossvars.push_back("blngrad");
  allowedcrossvars.push_back("ql");
  allowedcrossvars.push_back("qlpath");

  // Check input list of cross variables (crosslist)
  std::vector<std::string>::iterator it=crosslist.begin();
  while(it != crosslist.end())
  {
    if(!std::count(allowedcrossvars.begin(),allowedcrossvars.end(),*it))
    {
      if(master->mpiid == 0) std::printf("WARNING field %s in [thermo][crosslist] is illegal\n", it->c_str());
      it = crosslist.erase(it);  // erase() returns iterator of next element..
    }
    else
      ++it;
  }

  // Sort crosslist to group ql and b variables
  std::sort(crosslist.begin(),crosslist.end());

  return nerror;
}

int cthermo_moist::exec()
{
  int kk,nerror;
  kk = grid->icells*grid->jcells;
  int kcells = grid->kcells;
  nerror = 0;

  // Re-calculate hydrostatic pressure and exner, pass dummy as rhoref,thvref to prevent overwriting base state 
  double * restrict tmp2 = fields->s["tmp2"]->data;
  if(swupdatebasestate)
    calcbasestate(pref, prefh, &tmp2[0*kcells], &tmp2[1*kcells], &tmp2[2*kcells], &tmp2[3*kcells], exnref, exnrefh, 
                  fields->s["s"]->datamean, fields->s["qt"]->datamean);
  
  // extend later for gravity vector not normal to surface
  if(grid->swspatialorder == "2")
  {
    calcbuoyancytend_2nd(fields->wt->data, fields->s["s"]->data, fields->s["qt"]->data, prefh,
                         &fields->s["tmp2"]->data[0*kk], &fields->s["tmp2"]->data[1*kk], &fields->s["tmp2"]->data[2*kk],
                         thvrefh);
  }
  else if(grid->swspatialorder == "4")
  {
    calcbuoyancytend_4th(fields->wt->data, fields->s["s"]->data, fields->s["qt"]->data, prefh,
                         &fields->s["tmp2"]->data[0*kk], &fields->s["tmp2"]->data[1*kk], &fields->s["tmp2"]->data[2*kk],
                         thvrefh);
  }

  return (nerror>0);
}

int cthermo_moist::getmask(cfield3d *mfield, cfield3d *mfieldh, mask *m)
{
  if(m->name == "ql")
  {
    calcqlfield(fields->s["tmp1"]->data, fields->s["s"]->data, fields->s["qt"]->data, pref);
    calcmaskql(mfield->data, mfieldh->data,
               stats->nmask, stats->nmaskh,
               m->profs["area"].data, m->profs["areah"].data,
               fields->s["tmp1"]->data);
  }
  else if(m->name == "qlcore")
  {
    calcbuoyancy(fields->s["tmp2"]->data, fields->s["s"]->data, fields->s["qt"]->data, pref, fields->s["tmp1"]->data,thvref);
    // calculate the mean buoyancy to determine positive buoyancy
    grid->calcmean(fields->s["tmp2"]->datamean, fields->s["tmp2"]->data, grid->kcells);
    calcqlfield(fields->s["tmp1"]->data, fields->s["s"]->data, fields->s["qt"]->data, pref);
    calcmaskqlcore(mfield->data, mfieldh->data,
                   stats->nmask, stats->nmaskh,
                   m->profs["area"].data, m->profs["areah"].data,
                   fields->s["tmp1"]->data, fields->s["tmp2"]->data, fields->s["tmp2"]->datamean);
  }
 
  return 0;
}

int cthermo_moist::calcmaskql(double * restrict mask, double * restrict maskh,
                              int * restrict nmask, int * restrict nmaskh,
                              double * restrict area, double * restrict areah,
                              double * restrict ql)
{
  int ijk,jj,kk;
  int kstart,kend;

  jj = grid->icells;
  kk = grid->ijcells;
  kstart = grid->kstart;
  kend   = grid->kend;

  int ntmp;

  for(int k=grid->kstart; k<grid->kend; k++)
  {
    nmask[k] = 0;
    for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; i++)
      {
        ijk = i + j*jj + k*kk;
        ntmp = ql[ijk] > 0.;
        nmask[k] += ntmp;
        mask[ijk] = (double)ntmp;
      }
  }

  for(int k=grid->kstart; k<grid->kend+1; k++)
  {
    nmaskh[k] = 0;
    for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; i++)
      {
        ijk = i + j*jj + k*kk;
        ntmp = (ql[ijk-kk] + ql[ijk]) > 0.;
        nmaskh[k] += ntmp;
        maskh[ijk] = (double)ntmp;
      }
  }

  grid->boundary_cyclic(mask);
  grid->boundary_cyclic(maskh);

  master->sum(nmask , grid->kcells);
  master->sum(nmaskh, grid->kcells);

  nmaskh[kstart] = 0;
  nmaskh[kend  ] = 0;

  return 0;
}

int cthermo_moist::calcmaskqlcore(double * restrict mask, double * restrict maskh,
                                    int * restrict nmask, int * restrict nmaskh,
                                    double * restrict area, double * restrict areah,
                                    double * restrict ql, double * restrict b, double * restrict bmean)
{
  int ijk,jj,kk;
  int kstart,kend;

  jj = grid->icells;
  kk = grid->ijcells;
  kstart = grid->kstart;
  kend   = grid->kend;

  int ntmp;

  for(int k=grid->kstart; k<grid->kend; k++)
  {
    nmask[k] = 0;
    for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; i++)
      {
        ijk = i + j*jj + k*kk;
        ntmp = (ql[ijk] > 0.)*(b[ijk]-bmean[k] > 0.);
        nmask[k] += ntmp;
        mask[ijk] = (double)ntmp;
      }
  }

  for(int k=grid->kstart; k<grid->kend+1; k++)
  {
    nmaskh[k] = 0;
    for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; i++)
      {
        ijk = i + j*jj + k*kk;
        ntmp = (ql[ijk-kk]+ql[ijk] > 0.)*(b[ijk-kk]+b[ijk]-bmean[k-1]-bmean[k] > 0.);
        nmaskh[k] += ntmp;
        maskh[ijk] = (double)ntmp;
      }
  }

  grid->boundary_cyclic(mask);
  grid->boundary_cyclic(maskh);

  master->sum(nmask , grid->kcells);
  master->sum(nmaskh, grid->kcells);

  nmaskh[kstart] = 0;
  nmaskh[kend  ] = 0;

  return 0;
}

int cthermo_moist::execstats(mask *m)
{
  // calc the buoyancy and its surface flux for the profiles
  calcbuoyancy(fields->s["tmp1"]->data, fields->s["s"]->data, fields->s["qt"]->data, pref, fields->s["tmp2"]->data, thvref);
  calcbuoyancyfluxbot(fields->s["tmp1"]->datafluxbot, fields->s["s"]->databot, fields->s["s"]->datafluxbot, fields->s["qt"]->databot, fields->s["qt"]->datafluxbot, thvrefh);

  // define location
  const int sloc[] = {0,0,0};

  // mean
  stats->calcmean(fields->s["tmp1"]->data, m->profs["b"].data, NO_OFFSET, sloc,
                  fields->s["tmp3"]->data, stats->nmask);

  // moments
  for(int n=2; n<5; ++n)
  {
    std::stringstream ss;
    ss << n;
    std::string sn = ss.str();
    stats->calcmoment(fields->s["tmp1"]->data, m->profs["b"].data, m->profs["b"+sn].data, n, sloc,
                      fields->s["tmp3"]->data, stats->nmask);
  }

  // calculate the gradients
  if(grid->swspatialorder == "2")
    stats->calcgrad_2nd(fields->s["tmp1"]->data, m->profs["bgrad"].data, grid->dzhi, sloc,
                        fields->s["tmp4"]->data, stats->nmaskh);
  if(grid->swspatialorder == "4")
    stats->calcgrad_4th(fields->s["tmp1"]->data, m->profs["bgrad"].data, grid->dzhi4, sloc,
                        fields->s["tmp4"]->data, stats->nmaskh);

  // calculate turbulent fluxes
  if(grid->swspatialorder == "2")
    stats->calcflux_2nd(fields->s["tmp1"]->data, m->profs["b"].data, fields->w->data, m->profs["w"].data,
                        m->profs["bw"].data, fields->s["tmp2"]->data, sloc,
                        fields->s["tmp4"]->data, stats->nmaskh);
  if(grid->swspatialorder == "4")
    stats->calcflux_4th(fields->s["tmp1"]->data, fields->w->data, m->profs["bw"].data, fields->s["tmp2"]->data, sloc,
                        fields->s["tmp4"]->data, stats->nmaskh);

  // calculate diffusive fluxes
  if(model->diff->getname() == "les2s")
  {
    cdiff_les2s *diffptr = static_cast<cdiff_les2s *>(model->diff);
    stats->calcdiff_2nd(fields->s["tmp1"]->data, fields->w->data, fields->s["evisc"]->data,
                        m->profs["bdiff"].data, grid->dzhi,
                        fields->s["tmp1"]->datafluxbot, fields->s["tmp1"]->datafluxtop, diffptr->tPr, sloc,
                        fields->s["tmp4"]->data, stats->nmaskh);
  }
  else
  {
    // take the diffusivity of temperature for that of moisture
    stats->calcdiff_4th(fields->s["tmp1"]->data, m->profs["bdiff"].data, grid->dzhi4, fields->s["th"]->visc, sloc,
                        fields->s["tmp4"]->data, stats->nmaskh);
  }

  // calculate the total fluxes
  stats->addfluxes(m->profs["bflux"].data, m->profs["bw"].data, m->profs["bdiff"].data);

  // calculate the liquid water stats
  calcqlfield(fields->s["tmp1"]->data, fields->s["s"]->data, fields->s["qt"]->data, pref);
  stats->calcmean(fields->s["tmp1"]->data, m->profs["ql"].data, NO_OFFSET, sloc, fields->s["tmp3"]->data, stats->nmask);
  stats->calccount(fields->s["tmp1"]->data, m->profs["cfrac"].data, 0.,
                   fields->s["tmp3"]->data, stats->nmask);

  stats->calccover(fields->s["tmp1"]->data, &m->tseries["ccover"].data, 0.);
  stats->calcpath(fields->s["tmp1"]->data, &m->tseries["lwp"].data);

  return 0;
}

int cthermo_moist::execcross()
{
  int nerror = 0;

  // With one additional temp field, we wouldn't have to re-calculate the ql or b field for simple,lngrad,path, etc.
  for(std::vector<std::string>::iterator it=crosslist.begin(); it<crosslist.end(); ++it)
  {
    if(*it == "b" or *it == "ql")
    {
      getthermofield(fields->s["tmp1"], fields->s["tmp2"], *it);
      nerror += model->cross->crosssimple(fields->s["tmp1"]->data, fields->s["tmp2"]->data, *it);
    }
    else if(*it == "blngrad")
    {
      getthermofield(fields->s["tmp1"], fields->s["tmp2"], "b");
      // Note: tmp1 twice used as argument -> overwritten in crosspath()
      nerror += model->cross->crosslngrad(fields->s["tmp1"]->data, fields->s["tmp2"]->data, fields->s["tmp1"]->data, grid->dzi4, *it);
    }
    else if(*it == "qlpath")
    {
      getthermofield(fields->s["tmp1"], fields->s["tmp2"], "ql");
      // Note: tmp1 twice used as argument -> overwritten in crosspath()
      nerror += model->cross->crosspath(fields->s["tmp1"]->data, fields->s["tmp2"]->data, fields->s["tmp1"]->data, "qlpath");
    }
    else if(*it == "bbot" or *it == "bfluxbot")
    {
      getbuoyancysurf(fields->s["tmp1"]);
      if(*it == "bbot")
        nerror += model->cross->crossplane(fields->s["tmp1"]->databot, fields->s["tmp1"]->data, "bbot");
      else if(*it == "bfluxbot")
        nerror += model->cross->crossplane(fields->s["tmp1"]->datafluxbot, fields->s["tmp1"]->data, "bfluxbot");
    }
  }  

  return nerror; 
}

int cthermo_moist::checkthermofield(std::string name)
{
  if(name == "b" || name == "ql")
    return 0;
  else
    return 1;
}

int cthermo_moist::getthermofield(cfield3d *fld, cfield3d *tmp, std::string name)
{
  int kk = grid->icells*grid->jcells;
  int kcells = grid->kcells;

  // BvS: getthermofield() is called from subgrid-model, before thermo(), so re-calculate the hydrostatic pressure
  // Pass dummy as rhoref,thvref to prevent overwriting base state 
  double * restrict tmp2 = fields->s["tmp2"]->data;
  if(swupdatebasestate)
    calcbasestate(pref, prefh, &tmp2[0*kcells], &tmp2[1*kcells], &tmp2[2*kcells], &tmp2[3*kcells], exnref, exnrefh, 
                  fields->s["s"]->datamean, fields->s["qt"]->datamean);

  if(name == "b")
    calcbuoyancy(fld->data, fields->s["s"]->data, fields->s["qt"]->data, pref, tmp->data, thvref);
  else if(name == "ql")
    calcqlfield(fld->data, fields->s["s"]->data, fields->s["qt"]->data, pref);
  else if(name == "N2")
    calcN2(fld->data, fields->s["s"]->data, grid->dzi, thvref);
  else
    return 1;

  return 0;
}

int cthermo_moist::getbuoyancysurf(cfield3d *bfield)
{
  calcbuoyancybot(bfield->data         , bfield->databot,
                  fields->s["s" ]->data, fields->s["s" ]->databot,
                  fields->s["qt"]->data, fields->s["qt"]->databot,
                  thvref, thvrefh);
  calcbuoyancyfluxbot(bfield->datafluxbot, fields->s["s"]->databot, fields->s["s"]->datafluxbot, fields->s["qt"]->databot, fields->s["qt"]->datafluxbot, thvrefh);
  return 0;
}

int cthermo_moist::getbuoyancyfluxbot(cfield3d *bfield)
{
  calcbuoyancyfluxbot(bfield->datafluxbot, fields->s["s"]->databot, fields->s["s"]->datafluxbot, fields->s["qt"]->databot, fields->s["qt"]->datafluxbot, thvrefh);
  return 0;
}

int cthermo_moist::getprogvars(std::vector<std::string> *list)
{
  list->push_back("s");
  list->push_back("qt");

  return 0;
}

/**
 * This function calculates the hydrostatic pressure at full and half levels, 
 * with option to return base state profiles like reference density and temperature
 * Solves: dpi/dz=-g/thv with pi=cp*(p/p0)**(rd/cp)
 * @param pref Pointer to output hydrostatic pressure array (full level) 
 * @param prefh Pointer to output hydrostatic pressure array (half level) 
 * @param dn Pointer to output density array (full level) 
 * @param dnh Pointer to output density array (half level) 
 * @param thv Pointer to output virtual potential temperature array (full level) 
 * @param thvh Pointer to output virtual potential temperature array (half level) 
 * @param ex Pointer to output exner array (full level) 
 * @param exh Pointer to output exner array (half level) 
 * @param thlmean Pointer to input liq. water potential temperature array (horizontal mean, full level) 
 * @param qtmean Pointer to input tot. moisture mix. ratio  array (horizontal mean, full level) 
 * @return Returns 1 on error, 0 otherwise.
 */
int cthermo_moist::calcbasestate(double * restrict pref,     double * restrict prefh,
                                 double * restrict rho,      double * restrict rhoh,
                                 double * restrict thv,      double * restrict thvh,
                                 double * restrict ex,       double * restrict exh,
                                 double * restrict thlmean,  double * restrict qtmean)
{
  int kstart,kend;
  double ssurf,qtsurf,stop,qttop,ptop,ql,si,qti,qli,thvt;
  double rdcp = rd/cp;

  kstart = grid->kstart;
  kend = grid->kend;

  if(grid->swspatialorder == "2")
  {
    ssurf  = interp2(thlmean[kstart-1], thlmean[kstart]);
    stop   = interp2(thlmean[kend-1],   thlmean[kend]);
    qtsurf = interp2(qtmean[kstart-1],  qtmean[kstart]);
    qttop  = interp2(qtmean[kend-1],    qtmean[kend]);
  }
  else if(grid->swspatialorder == "4")
  {
    ssurf  = interp4(thlmean[kstart-2], thlmean[kstart-1], thlmean[kstart], thlmean[kstart+1]);
    stop   = interp4(thlmean[kend-2],   thlmean[kend-1],   thlmean[kend],   thlmean[kend+1]);
    qtsurf = interp4(qtmean[kstart-2],  qtmean[kstart-1],  qtmean[kstart],  qtmean[kstart+1]);
    qttop  = interp4(qtmean[kend-2],    qtmean[kend-1],    qtmean[kend],    qtmean[kend+1]);
  }

  // Calculate surface (half=kstart) values
  exh[kstart]   = exn(ps);
  ql            = calcql(ssurf,qtsurf,ps,exh[kstart]); 
  thvh[kstart]  = (ssurf + lv*ql/(cp*exh[kstart])) * (1. - (1. - rv/rd)*qtsurf - rv/rd*ql);
  prefh[kstart] = ps;
  rhoh[kstart]  = ps / (rd * exh[kstart] * thvh[kstart]);

  // First full grid level pressure
  pref[kstart] = pow((pow(ps,rdcp) - grav * pow(p0,rdcp) * grid->z[kstart] / (cp * thvh[kstart])),(1./rdcp)); 

  for(int k=kstart+1; k<kend+1; k++)
  {
    // 1. Calculate values at full level below zh[k] 
    ex[k-1]  = exn(pref[k-1]);
    ql       = calcql(thlmean[k-1],qtmean[k-1],pref[k-1],ex[k-1]); 
    thv[k-1] = (thlmean[k-1] + lv*ql/(cp*ex[k-1])) * (1. - (1. - rv/rd)*qtmean[k-1] - rv/rd*ql); 
    rho[k-1] = pref[k-1] / (rd * ex[k-1] * thv[k-1]);
 
    // 2. Calculate half level pressure at zh[k] using values at z[k-1]
    prefh[k] = pow((pow(prefh[k-1],rdcp) - grav * pow(p0,rdcp) * grid->dz[k-1] / (cp * thv[k-1])),(1./rdcp));

    // 3. Interpolate conserved variables to zh[k] and calculate virtual temp and ql
    if(grid->swspatialorder == "2")
    {
      si     = interp2(thlmean[k-1],thlmean[k]);
      qti    = interp2(qtmean[k-1],qtmean[k]);
    }
    else if(grid->swspatialorder == "4")
    {
      si     = interp4(thlmean[k-2],thlmean[k-1],thlmean[k],thlmean[k+1]);
      qti    = interp4(qtmean[k-2],qtmean[k-1],qtmean[k],qtmean[k+1]);
    }

    exh[k]   = exn(prefh[k]);
    qli      = calcql(si,qti,prefh[k],exh[k]);
    thvh[k]  = (si + lv*qli/(cp*exh[k])) * (1. - (1. - rv/rd)*qti - rv/rd*qli); 
    rhoh[k]  = prefh[k] / (rd * exh[k] * thvh[k]); 

    // 4. Calculate full level pressure at z[k]
    pref[k]  = pow((pow(pref[k-1],rdcp) - grav * pow(p0,rdcp) * grid->dzh[k] / (cp * thvh[k])),(1./rdcp)); 
  }

  // Fill bottom and top full level ghost cells 
  if(grid->swspatialorder == "2")
  {
    pref[kstart-1] = 2.*prefh[kstart] - pref[kstart];
    pref[kend]     = 2.*prefh[kend]   - pref[kend-1];
  }
  else if(grid->swspatialorder == "4")
  {
    pref[kstart-1] = (8./3.)*prefh[kstart] - 2.*pref[kstart] + (1./3.)*pref[kstart+1];
    pref[kstart-2] = 8.*prefh[kstart]      - 9.*pref[kstart] + 2.*pref[kstart+1];
    pref[kend]     = (8./3.)*prefh[kend]   - 2.*pref[kend-1] + (1./3.)*pref[kend-2];
    pref[kend+1]   = 8.*prefh[kend]        - 9.*pref[kend-1] + 2.*pref[kend-2];
  }

  return 0;
}

int cthermo_moist::calcbuoyancytend_2nd(double * restrict wt, double * restrict s, double * restrict qt, 
                                        double * restrict ph, double * restrict sh, double * restrict qth, double * restrict ql,
                                        double * restrict thvrefh)
{
  int ijk,jj,kk,ij;
  double tl, exnh;
  jj = grid->icells;
  kk = grid->icells*grid->jcells;

  // CvH check the usage of the gravity term here, in case of scaled DNS we use one. But thermal expansion coeff??
  for(int k=grid->kstart+1; k<grid->kend; k++)
  {
    //ph   = interp2(p[k-1],p[k]);   // BvS To-do: calculate pressure at full and half levels
    exnh = exn(ph[k]);
    for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; i++)
      {
        ijk = i + j*jj + k*kk;
        ij  = i + j*jj;
        sh[ij]  = interp2(s[ijk-kk], s[ijk]);
        qth[ij] = interp2(qt[ijk-kk], qt[ijk]);
        tl      = sh[ij] * exnh;
        // Calculate first estimate of ql using Tl
        // if ql(Tl)>0, saturation adjustment routine needed
        ql[ij]  = qth[ij]-rslf(ph[k],tl);
      }
    for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; i++)
      {
        ij  = i + j*jj;
        if(ql[ij]>0)   // already doesn't vectorize because of iteration in calcql()
          ql[ij] = calcql(sh[ij], qth[ij], ph[k], exnh);
        else
          ql[ij] = 0.;
      }
    for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; i++)
      {
        ijk = i + j*jj + k*kk;
        ij  = i + j*jj;
        wt[ijk] += bu(ph[k], sh[ij], qth[ij], ql[ij], thvrefh[k]);
      }
  }
  return 0;
}

int cthermo_moist::calcbuoyancytend_4th(double * restrict wt, double * restrict s, double * restrict qt, 
                                        double * restrict ph, double * restrict sh, double * restrict qth, double * restrict ql,
                                        double * restrict thvrefh)
{
  int ijk,jj,ij;
  int kk1,kk2;
  double tl, exnh;

  jj  = grid->icells;
  kk1 = 1*grid->icells*grid->jcells;
  kk2 = 2*grid->icells*grid->jcells;

  for(int k=grid->kstart+1; k<grid->kend; k++)
  {
    exnh = exn(ph[k]);
    for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; i++)
      {
        ijk = i + j*jj + k*kk1;
        ij  = i + j*jj;
        sh[ij]  = interp4(s[ijk-kk2] , s[ijk-kk1] , s[ijk] , s[ijk+kk1]);
        qth[ij] = interp4(qt[ijk-kk2], qt[ijk-kk1], qt[ijk], qt[ijk+kk1]);
        tl      = sh[ij] * exnh;
        // Calculate first estimate of ql using Tl
        // if ql(Tl)>0, saturation adjustment routine needed
        ql[ij]  = qth[ij]-rslf(ph[k],tl);   
      }
    for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; i++)
      {
        ij  = i + j*jj;
        if(ql[ij]>0)   // already doesn't vectorize because of iteration in calcql()
          ql[ij] = calcql(sh[ij], qth[ij], ph[k], exnh);
        else
          ql[ij] = 0.;
      }
    for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; i++)
      {
        ijk = i + j*jj + k*kk1;
        ij  = i + j*jj;
        wt[ijk] += bu(ph[k], sh[ij], qth[ij], ql[ij], thvrefh[k]);
      }
  }
  return 0;
}

int cthermo_moist::calcbuoyancy(double * restrict b, double * restrict s, double * restrict qt, double * restrict p, double * restrict ql,
                                double * restrict thvref)
{
  int ijk,jj,kk,ij;
  double tl, ex;
  jj = grid->icells;
  kk = grid->icells*grid->jcells;

  for(int k=0; k<grid->kcells; k++)
  {
    ex = exn(p[k]);
    for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; i++)
      {
        ijk = i + j*jj + k*kk;
        ij  = i + j*jj;
        tl  = s[ijk] * ex;
        ql[ij]  = qt[ijk]-rslf(p[k],tl);   // not real ql, just estimate
      }
    for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; i++)
      {
        ijk = i + j*jj + k*kk;
        ij  = i + j*jj;
        if(ql[ij] > 0)
          ql[ij] = calcql(s[ijk], qt[ijk], p[k], ex);
        else
          ql[ij] = 0.;
      }
    for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; i++)
      {
        ijk = i + j*jj + k*kk;
        ij  = i + j*jj;
        b[ijk] = bu(p[k], s[ijk], qt[ijk], ql[ij], thvref[k]);
      }
  }

  return 0;
}

int cthermo_moist::calcqlfield(double * restrict ql, double * restrict s, double * restrict qt, double * restrict p)
{
  int ijk,jj,kk;
  double ex;

  jj = grid->icells;
  kk = grid->icells*grid->jcells;

  for(int k=grid->kstart; k<grid->kend; k++)
  {
    ex = exn(p[k]);
    for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; i++)
      {
        ijk = i + j*jj + k*kk;
        ql[ijk] = calcql(s[ijk], qt[ijk], p[k], ex);
      }
  }
  return 0;
}

int cthermo_moist::calcN2(double * restrict N2, double * restrict s, double * restrict dzi, double * restrict thvref)
{
  int ijk,jj,kk;
  jj = grid->icells;
  kk = grid->icells*grid->jcells;

  // double thvref = thvs;

  for(int k=0; k<grid->kcells; ++k)
    for(int j=grid->jstart; j<grid->jend; ++j)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; ++i)
      {
        ijk = i + j*jj + k*kk;
        N2[ijk] = grav/thvref[k]*0.5*(s[ijk+kk] - s[ijk-kk])*dzi[k];
      }

  return 0;
}

int cthermo_moist::calcbuoyancybot(double * restrict b , double * restrict bbot,
                                   double * restrict s , double * restrict sbot,
                                   double * restrict qt, double * restrict qtbot,
                                   double * restrict thvref, double * restrict thvrefh)
{
  int ij,ijk,jj,kk,kstart;
  jj = grid->icells;
  kk = grid->icells*grid->jcells;
  kstart = grid->kstart;

  // double thvref = thvs;

  // assume no liquid water at the lowest model level
  for(int j=0; j<grid->jcells; j++)
#pragma ivdep
    for(int i=0; i<grid->icells; i++)
    {
      ij  = i + j*jj;
      ijk = i + j*jj + kstart*kk;
      bbot[ij ] = bunoql(sbot[ij], qtbot[ij], thvrefh[kstart]);
      b   [ijk] = bunoql(s[ijk], qt[ijk], thvref[kstart]);
    }

  return 0;
}

int cthermo_moist::calcbuoyancyfluxbot(double * restrict bfluxbot, double * restrict sbot, double * restrict sfluxbot, double * restrict qtbot, double * restrict qtfluxbot,
                                       double * restrict thvrefh)
{
  int ij,jj,kstart;
  jj = grid->icells;
  kstart = grid->kstart;

  // double thvref = thvs;

  // assume no liquid water at the lowest model level
  for(int j=0; j<grid->jcells; j++)
#pragma ivdep
    for(int i=0; i<grid->icells; i++)
    {
      ij  = i + j*jj;
      bfluxbot[ij] = bufluxnoql(sbot[ij], sfluxbot[ij], qtbot[ij], qtfluxbot[ij], thvrefh[kstart]);
    }

  return 0;
}

// INLINE FUNCTIONS
inline double cthermo_moist::bu(const double p, const double s, const double qt, const double ql, const double thvref)
{
  return grav * ((s + lv*ql/(cp*exn(p))) * (1. - (1. - rv/rd)*qt - rv/rd*ql) - thvref) / thvref;
}

inline double cthermo_moist::bunoql(const double s, const double qt, const double thvref)
{
  return grav * (s * (1. - (1. - rv/rd)*qt) - thvref) / thvref;
}

inline double cthermo_moist::bufluxnoql(const double s, const double sflux, const double qt, const double qtflux, const double thvref)
{
  return grav/thvref * (sflux * (1. - (1.-rv/rd)*qt) - (1.-rv/rd)*s*qtflux);
}

inline double cthermo_moist::calcql(const double s, const double qt, const double p, const double exn)
{
  int niter = 0; //, nitermax = 5;
  double ql, tl, tnr_old = 1.e9, tnr, qs;
  tl = s * exn;
  tnr = tl;
  while (std::fabs(tnr-tnr_old)/tnr_old> 1e-5)// && niter < nitermax)
  {
    ++niter;
    tnr_old = tnr;
    qs = rslf(p,tnr);
    tnr = tnr - (tnr+(lv/cp)*qs-tl-(lv/cp)*qt)/(1+(std::pow(lv,2)*qs)/ (rv*cp*std::pow(tnr,2)));
  }
  ql = std::max(0.,qt - qs);
  return ql;
}

inline double cthermo_moist::exn(const double p)
{
  return pow((p/p0),(rd/cp));
}

inline double cthermo_moist::exn2(const double p)
{
  double dp=p-p0;
  return (1+(dp*(ex1+dp*(ex2+dp*(ex3+dp*(ex4+dp*(ex5+dp*(ex6+ex7*dp)))))))); 
}

inline double cthermo_moist::rslf(const double p, const double t)
{
  return ep*esl(t)/(p-(1-ep)*esl(t));
}

inline double cthermo_moist::esl(const double t)
{
  const double x=std::max(-80.,t-tmelt);
  return c0+x*(c1+x*(c2+x*(c3+x*(c4+x*(c5+x*(c6+x*(c7+x*c8)))))));

  //return es0*std::exp(at*(t-tmelt)/(t-bt));
}

inline double cthermo_moist::interp2(const double a, const double b)
{
  return 0.5*(a + b);
}

inline double cthermo_moist::interp4(const double a, const double b, const double c, const double d)
{
  return (-a + 9.*b + 9.*c - d) / 16.;
}


