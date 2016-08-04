/*
 * Hydro.cc
 *
 *  Created on: Dec 22, 2011
 *      Author: cferenba
 *
 * Copyright (c) 2012, Los Alamos National Security, LLC.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style open-source
 * license; see top-level LICENSE file for full license text.
 */

#include "Hydro.hh"

#include <string>
#include <vector>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <iomanip>

#include "Parallel.hh"
#include "Memory.hh"
#include "Mesh.hh"
#include "PolyGas.hh"
#include "TTS.hh"
#include "QCS.hh"
#include "HydroBC.hh"

using namespace std;

// JPG TODO: declare const initialized in all constructors as const
Hydro::Hydro(const InputParameters& params, Mesh* m) :
		mesh(m),
		cfl(params.cfl_),
		cflv(params.cflv_),
		rho_init(params.rho_init_),
		energy_init(params.energy_init_),
		rho_init_sub(params.rho_init_sub_),
		energy_init_sub(params.energy_init_sub_),
		vel_init_radial(params.vel_init_radial_),
		bcx(params.bcx_),
		bcy(params.bcy_)
{
    pgas = new PolyGas(params, this);
    tts = new TTS(params, this);
    qcs = new QCS(params, this);

    const double2 vfixx = double2(1., 0.);
    const double2 vfixy = double2(0., 1.);
    for (int i = 0; i < bcx.size(); ++i)
        bcs.push_back(new HydroBC(mesh, vfixx, mesh->getXPlane(bcx[i])));
    for (int i = 0; i < bcy.size(); ++i)
        bcs.push_back(new HydroBC(mesh, vfixy, mesh->getYPlane(bcy[i])));

    init();
}


Hydro::~Hydro() {

    delete tts;
    delete qcs;
    for (int i = 0; i < bcs.size(); ++i) {
        delete bcs[i];
    }
}


void Hydro::init() {

    const int numpch = mesh->num_pt_chunks;
    const int numzch = mesh->num_zone_chunks;
    const int nump = mesh->num_pts_;
    const int numz = mesh->num_zones_;
    const int nums = mesh->num_sides_;

    const double2* zx = mesh->zone_x_;
    const double* zvol = mesh->zone_vol_;

    // allocate arrays
    pt_vel = AbstractedMemory::alloc<double2>(nump);
    pt_vel0 = AbstractedMemory::alloc<double2>(nump);
    pt_accel = AbstractedMemory::alloc<double2>(nump);
    pt_force = AbstractedMemory::alloc<double2>(nump);
    pt_weighted_mass = AbstractedMemory::alloc<double>(nump);
    crnr_weighted_mass = AbstractedMemory::alloc<double>(nums);
    zone_mass = AbstractedMemory::alloc<double>(numz);
    zone_rho = AbstractedMemory::alloc<double>(numz);
    zone_rho_pred = AbstractedMemory::alloc<double>(numz);
    zone_energy_density = AbstractedMemory::alloc<double>(numz);
    zone_energy_tot = AbstractedMemory::alloc<double>(numz);
    zone_work = AbstractedMemory::alloc<double>(numz);
    zone_work_rate = AbstractedMemory::alloc<double>(numz);
    zone_pres = AbstractedMemory::alloc<double>(numz);
    zone_sound_speed = AbstractedMemory::alloc<double>(numz);
    zone_dvel = AbstractedMemory::alloc<double>(numz);
    side_force_pres = AbstractedMemory::alloc<double2>(nums);
    side_force_visc = AbstractedMemory::alloc<double2>(nums);
    side_force_tts = AbstractedMemory::alloc<double2>(nums);
    crnr_force_tot = AbstractedMemory::alloc<double2>(nums);

    // initialize hydro vars
    for (int zch = 0; zch < numzch; ++zch) {
        int zfirst = mesh->zone_chunk_first[zch];
        int zlast = mesh->zone_chunk_last[zch];

        fill(&zone_rho[zfirst], &zone_rho[zlast], rho_init);
        fill(&zone_energy_density[zfirst], &zone_energy_density[zlast], energy_init);
        fill(&zone_work_rate[zfirst], &zone_work_rate[zlast], 0.);

        const vector<double>& subrgn = mesh->subregion_;
        if (!subrgn.empty()) {
            const double eps = 1.e-12;
            #pragma ivdep
            for (int z = zfirst; z < zlast; ++z) {
                if (zx[z].x > (subrgn[0] - eps) &&
                    zx[z].x < (subrgn[1] + eps) &&
                    zx[z].y > (subrgn[2] - eps) &&
                    zx[z].y < (subrgn[3] + eps)) {
                    zone_rho[z] = rho_init_sub;
                    zone_energy_density[z] = energy_init_sub;
                }
            }
        }

        #pragma ivdep
        for (int z = zfirst; z < zlast; ++z) {
            zone_mass[z] = zone_rho[z] * zvol[z];
            zone_energy_tot[z] = zone_energy_density[z] * zone_mass[z];
        }
    }  // for sch

    for (int pch = 0; pch < numpch; ++pch) {
        int pfirst = mesh->pt_chunks_first[pch];
        int plast = mesh->pt_chunks_last[pch];
        if (vel_init_radial != 0.)
            initRadialVel(vel_init_radial, pfirst, plast);
        else
            fill(&pt_vel[pfirst], &pt_vel[plast], double2(0., 0.));
    }  // for pch

    resetDtHydro();

}


void Hydro::initRadialVel(
        const double vel,
        const int pfirst,
        const int plast) {
    const double2* px = mesh->pt_x_;
    const double eps = 1.e-12;

    #pragma ivdep
    for (int p = pfirst; p < plast; ++p) {
        double pmag = length(px[p]);
        if (pmag > eps)
            pt_vel[p] = vel * px[p] / pmag;
        else
            pt_vel[p] = double2(0., 0.);
    }
}


void Hydro::doCycle(
            const double dt) {

    const int num_pt_chunks = mesh->num_pt_chunks;
    const int num_side_chunks = mesh->num_side_chunks;

    // Begin hydro cycle
    for (int pt_chunk = 0; pt_chunk < num_pt_chunks; ++pt_chunk) {
        int pt_first = mesh->pt_chunks_first[pt_chunk];
        int pt_last = mesh->pt_chunks_last[pt_chunk];

        // save off point variable values from previous cycle
        copy(&mesh->pt_x_[pt_first], &mesh->pt_x_[pt_last], &mesh->pt_x0[pt_first]);
        copy(&pt_vel[pt_first], &pt_vel[pt_last], &pt_vel0[pt_first]);

        // ===== Predictor step =====
        // 1. advance mesh to center of time step
        advPosHalf(mesh->pt_x0, pt_vel0, dt, mesh->pt_x_pred, pt_first, pt_last);
    } // for pch

    for (int sch = 0; sch < num_side_chunks; ++sch) {
        int sfirst = mesh->side_chunks_first[sch];
        int slast = mesh->side_chunks_last[sch];
        int zfirst = mesh->zone_chunks_first[sch];
        int zlast = mesh->zone_chunks_last[sch];

        // save off zone variable values from previous cycle
        copy(&mesh->zone_vol_[zfirst], &mesh->zone_vol_[zlast], &mesh->zone_vol0[zfirst]);

        // 1a. compute new mesh geometry
        mesh->calcCtrs(sch);
        mesh->calcVols(sch);
        mesh->calcMedianMeshSurfVecs(sch);
        mesh->calcEdgeLen(sch);
        mesh->calcCharacteristicLen(sch);

        // 2. compute point masses
        calcRho(zone_mass, mesh->zone_vol_pred, zone_rho_pred, zfirst, zlast);
        calcCrnrMass(zone_rho_pred, mesh->zone_area_pred, mesh->side_mass_frac, crnr_weighted_mass, sfirst, slast);

        // 3. compute material state (half-advanced)
        pgas->calcStateAtHalf(zone_rho, mesh->zone_vol_pred, mesh->zone_vol0, zone_energy_density, zone_work_rate, zone_mass, dt,
                zone_pres, zone_sound_speed, zfirst, zlast);

        // 4. compute forces
        pgas->calcForce(zone_pres, mesh->side_surfp, side_force_pres, sfirst, slast);
        tts->calcForce(mesh->zone_area_pred, zone_rho_pred, zone_sound_speed, mesh->side_area_pred, mesh->side_mass_frac, mesh->side_surfp, side_force_tts,
                sfirst, slast);
        qcs->calcForce(side_force_visc, sfirst, slast);
        sumCrnrForce(side_force_pres, side_force_visc, side_force_tts, crnr_force_tot, sfirst, slast);
    }  // for sch
    mesh->checkBadSides();

    // sum corner masses, forces to points
    mesh->sumToPoints(crnr_weighted_mass, pt_weighted_mass);
    mesh->sumToPoints(crnr_force_tot, pt_force);

    for (int pch = 0; pch < num_pt_chunks; ++pch) {
        int pfirst = mesh->pt_chunks_first[pch];
        int plast = mesh->pt_chunks_last[pch];

        // 4a. apply boundary conditions
        for (int i = 0; i < bcs.size(); ++i) {
            int bfirst = bcs[i]->pchbfirst[pch];
            int blast = bcs[i]->pchblast[pch];
            bcs[i]->applyFixedBC(pt_vel0, pt_force, bfirst, blast);
        }

        // 5. compute accelerations
        calcAccel(pt_force, pt_weighted_mass, pt_accel, pfirst, plast);

        // ===== Corrector step =====
        // 6. advance mesh to end of time step
        advPosFull(mesh->pt_x0, pt_vel0, pt_accel, dt, mesh->pt_x_, pt_vel, pfirst, plast);
    }  // for pch

    resetDtHydro();

    for (int sch = 0; sch < num_side_chunks; ++sch) {
        int sfirst = mesh->side_chunks_first[sch];
        int slast = mesh->side_chunks_last[sch];
        int zfirst = mesh->zone_chunks_first[sch];
        int zlast = mesh->zone_chunks_last[sch];

        // 6a. compute new mesh geometry
        mesh->calcCtrs(sch, false);
        mesh->calcVols(sch, false);

        // 7. compute work
        fill(&zone_work[zfirst], &zone_work[zlast], 0.);
        calcWork(side_force_pres, side_force_visc, pt_vel0, pt_vel, mesh->pt_x_pred, dt, zone_work, zone_energy_tot,
                sfirst, slast);
    }  // for sch
    mesh->checkBadSides();

    for (int zch = 0; zch < mesh->num_zone_chunks; ++zch) {
        int zfirst = mesh->zone_chunk_first[zch];
        int zlast = mesh->zone_chunk_last[zch];

        // 7a. compute work rate
        calcWorkRate(mesh->zone_vol0, mesh->zone_vol_, zone_work, zone_pres, dt, zone_work_rate, zfirst, zlast);

        // 8. update state variables
        calcEnergy(zone_energy_tot, zone_mass, zone_energy_density, zfirst, zlast);
        calcRho(zone_mass, mesh->zone_vol_, zone_rho, zfirst, zlast);

        // 9.  compute timestep for next cycle
        calcDtHydro(mesh->zone_vol_, mesh->zone_vol0, dt, zfirst, zlast);
    }  // for zch

}


void Hydro::advPosHalf(
        const double2* px0,
        const double2* pu0,
        const double dt,
        double2* pxp,
        const int pfirst,
        const int plast) {

    double dth = 0.5 * dt;

    #pragma ivdep
    for (int p = pfirst; p < plast; ++p) {
        pxp[p] = px0[p] + pu0[p] * dth;
    }
}


void Hydro::advPosFull(
        const double2* px0,
        const double2* pu0,
        const double2* pa,
        const double dt,
        double2* px,
        double2* pu,
        const int pfirst,
        const int plast) {

    #pragma ivdep
    for (int p = pfirst; p < plast; ++p) {
        pu[p] = pu0[p] + pa[p] * dt;
        px[p] = px0[p] + 0.5 * (pu[p] + pu0[p]) * dt;
    }

}


void Hydro::calcCrnrMass(
        const double* zr,
        const double* zarea,
        const double* side_mass_frac,
        double* cmaswt,
        const int sfirst,
        const int slast) {

    #pragma ivdep
    for (int s = sfirst; s < slast; ++s) {
        int s3 = mesh->mapSideToSidePrev(s);
        int z = mesh->map_side2zone_[s];

        double m = zr[z] * zarea[z] * 0.5 * (side_mass_frac[s] + side_mass_frac[s3]);
        cmaswt[s] = m;
    }
}


void Hydro::sumCrnrForce(
        const double2* sf,
        const double2* sf2,
        const double2* sf3,
        double2* cftot,
        const int sfirst,
        const int slast) {

    #pragma ivdep
    for (int s = sfirst; s < slast; ++s) {
        int s3 = mesh->mapSideToSidePrev(s);

        double2 f = (sf[s] + sf2[s] + sf3[s]) -
                    (sf[s3] + sf2[s3] + sf3[s3]);
        cftot[s] = f;
    }
}


void Hydro::calcAccel(
        const double2* pf,
        const double* pmass,
        double2* pa,
        const int pfirst,
        const int plast) {

    const double fuzz = 1.e-99;

    #pragma ivdep
    for (int p = pfirst; p < plast; ++p) {
        pa[p] = pf[p] / max(pmass[p], fuzz);
    }

}


void Hydro::calcRho(
        const double* zm,
        const double* zvol,
        double* zr,
        const int zfirst,
        const int zlast) {

    #pragma ivdep
    for (int z = zfirst; z < zlast; ++z) {
        zr[z] = zm[z] / zvol[z];
    }

}


void Hydro::calcWork(
        const double2* sf,
        const double2* sf2,
        const double2* pu0,
        const double2* pu,
        const double2* px,
        const double dt,
        double* zw,
        double* zetot,
        const int sfirst,
        const int slast) {

    // Compute the work done by finding, for each element/node pair,
    //   dwork= force * vavg
    // where force is the force of the element on the node
    // and vavg is the average velocity of the node over the time period

    const double dth = 0.5 * dt;

    for (int s = sfirst; s < slast; ++s) {
        int p1 = mesh->map_side2pt1_[s];
        int p2 = mesh->mapSideToPt2(s);
        int z = mesh->map_side2zone_[s];

        double2 sftot = sf[s] + sf2[s];
        double sd1 = dot( sftot, (pu0[p1] + pu[p1]));
        double sd2 = dot(-sftot, (pu0[p2] + pu[p2]));
        double dwork = -dth * (sd1 * px[p1].x + sd2 * px[p2].x);

        zetot[z] += dwork;
        zw[z] += dwork;

    }

}


void Hydro::calcWorkRate(
        const double* zvol0,
        const double* zvol,
        const double* zw,
        const double* zp,
        const double dt,
        double* zwrate,
        const int zfirst,
        const int zlast) {
    double dtinv = 1. / dt;
    #pragma ivdep
    for (int z = zfirst; z < zlast; ++z) {
        double dvol = zvol[z] - zvol0[z];
        zwrate[z] = (zw[z] + zp[z] * dvol) * dtinv;
    }

}


void Hydro::calcEnergy(
        const double* zetot,
        const double* zm,
        double* ze,
        const int zfirst,
        const int zlast) {

    const double fuzz = 1.e-99;
    #pragma ivdep
    for (int z = zfirst; z < zlast; ++z) {
        ze[z] = zetot[z] / (zm[z] + fuzz);
    }

}


void Hydro::sumEnergy(
        const double* zetot,
        const double* zarea,
        const double* zvol,
        const double* zm,
        const double* side_mass_frac,
        const double2* px,
        const double2* pu,
        double& ei,
        double& ek,
        const int zfirst,
        const int zlast,
        const int sfirst,
        const int slast) {

    // compute internal energy
    double sumi = 0.; 
    for (int z = zfirst; z < zlast; ++z) {
        sumi += zetot[z];
    }
    // multiply by 2\pi for cylindrical geometry
    ei += sumi * 2 * M_PI;

    // compute kinetic energy
    // in each individual zone:
    // zone ke = zone mass * (volume-weighted average of .5 * u ^ 2)
    //         = zm sum(c in z) [cvol / zvol * .5 * u ^ 2]
    //         = sum(c in z) [zm * cvol / zvol * .5 * u ^ 2]
    double sumk = 0.; 
    for (int s = sfirst; s < slast; ++s) {
        int s3 = mesh->mapSideToSidePrev(s);
        int p1 = mesh->map_side2pt1_[s];
        int z = mesh->map_side2zone_[s];

        double cvol = zarea[z] * px[p1].x * 0.5 * (side_mass_frac[s] + side_mass_frac[s3]);
        double cke = zm[z] * cvol / zvol[z] * 0.5 * length2(pu[p1]);
        sumk += cke;
    }
    // multiply by 2\pi for cylindrical geometry
    ek += sumk * 2 * M_PI;

}


void Hydro::calcDtCourant(
        const double* zdl,
        double& dtrec,
        char* msgdtrec,
        const int zfirst,
        const int zlast) {

    const double fuzz = 1.e-99;
    double dtnew = 1.e99;
    int zmin = -1;
    for (int z = zfirst; z < zlast; ++z) {
        double cdu = max(zone_dvel[z], max(zone_sound_speed[z], fuzz));
        double zdthyd = zdl[z] * cfl / cdu;
        zmin = (zdthyd < dtnew ? z : zmin);
        dtnew = (zdthyd < dtnew ? zdthyd : dtnew);
    }

    if (dtnew < dtrec) {
        dtrec = dtnew;
        snprintf(msgdtrec, 80, "Hydro Courant limit for z = %d", zmin);
    }

}


void Hydro::calcDtVolume(
        const double* zvol,
        const double* zvol0,
        const double dtlast,
        double& dtrec,
        char* msgdtrec,
        const int zfirst,
        const int zlast) {

    double dvovmax = 1.e-99;
    int zmax = -1;
    for (int z = zfirst; z < zlast; ++z) {
        double zdvov = abs((zvol[z] - zvol0[z]) / zvol0[z]);
        zmax = (zdvov > dvovmax ? z : zmax);
        dvovmax = (zdvov > dvovmax ? zdvov : dvovmax);
    }
    double dtnew = dtlast * cflv / dvovmax;
    if (dtnew < dtrec) {
        dtrec = dtnew;
        snprintf(msgdtrec, 80, "Hydro dV/V limit for z = %d", zmax);
    }

}


void Hydro::calcDtHydro(
        const double* zvol,
        const double* zvol0,
        const double dtlast,
        const int zfirst,
        const int zlast) {

    double dtchunk = 1.e99;
    char msgdtchunk[80];

    calcDtCourant(mesh->zone_dl, dtchunk, msgdtchunk, zfirst, zlast);
    calcDtVolume(zvol, zvol0, dtlast, dtchunk, msgdtchunk,
            zfirst, zlast);
    if (dtchunk < dt_recommend) {
        {
            // redundant test needed to avoid race condition
            if (dtchunk < dt_recommend) {
                dt_recommend = dtchunk;
                strncpy(dt_recommend_mesg, msgdtchunk, 80);
            }
        }
    }

}


void Hydro::getDtHydro(
        double& dtnew,
        string& msgdtnew) {

    if (dt_recommend < dtnew) {
        dtnew = dt_recommend;
        msgdtnew = string(dt_recommend_mesg);
    }

}


void Hydro::resetDtHydro() {

    dt_recommend = 1.e99;
    strcpy(dt_recommend_mesg, "Hydro default");

}


void Hydro::writeEnergyCheck() {

    using Parallel::mype;
    
    double ei = 0.;
    double ek = 0.;
    for (int sch = 0; sch < mesh->num_side_chunks; ++sch) {
        int sfirst = mesh->side_chunks_first[sch];
        int slast = mesh->side_chunks_last[sch];
        int zfirst = mesh->zone_chunks_first[sch];
        int zlast = mesh->zone_chunks_last[sch];

        double eichunk = 0.;
        double ekchunk = 0.;
        sumEnergy(zone_energy_tot, mesh->zone_area_, mesh->zone_vol_, zone_mass, mesh->side_mass_frac,
                mesh->pt_x_, pt_vel, eichunk, ekchunk,
                zfirst, zlast, sfirst, slast);
        {
            ei += eichunk;
            ek += ekchunk;
        }
    }

    Parallel::globalSum(ei);
    Parallel::globalSum(ek);

    if (mype == 0) {
        cout << scientific << setprecision(6);
        cout << "Energy check:  "
             << "total energy  = " << setw(14) << ei + ek << endl;
        cout << "(internal = " << setw(14) << ei
             << ", kinetic = " << setw(14) << ek << ")" << endl;
    }

 }
